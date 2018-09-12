#include <inttypes.h>
#include <atomic>
#include <utility>
#include <vector>

#include "Inline/BasicTypes.h"
#include "Inline/Errors.h"
#include "Inline/Hash.h"
#include "Inline/HashSet.h"
#include "Inline/Lock.h"
#include "Inline/Timing.h"
#include "Logging/Logging.h"
#include "Platform/Mutex.h"
#include "Runtime/Runtime.h"
#include "RuntimePrivate.h"

using namespace Runtime;

// Keep a global list of all objects.
struct GCGlobals
{
	Platform::Mutex mutex;
	HashSet<ObjectImpl*> allObjects;

	static GCGlobals& get()
	{
		static GCGlobals globals;
		return globals;
	}

private:
	GCGlobals() {}
};

Runtime::ObjectImpl::ObjectImpl(ObjectKind inKind) : Object(inKind), numRootReferences(0)
{
	// Add the object to the global array.
	Lock<Platform::Mutex> lock(GCGlobals::get().mutex);
	GCGlobals::get().allObjects.addOrFail(this);
}

void Runtime::addGCRoot(Object* object)
{
	ObjectImpl* gcObject = (ObjectImpl*)object;
	++gcObject->numRootReferences;
}

void Runtime::removeGCRoot(Object* object)
{
	ObjectImpl* gcObject = (ObjectImpl*)object;
	--gcObject->numRootReferences;
}

static void visitReference(HashSet<ObjectImpl*>& unreferencedObjects,
						   std::vector<Object*>& pendingScanObjects,
						   Object* reference)
{
	if(reference && unreferencedObjects.remove((ObjectImpl*)reference))
	{ pendingScanObjects.push_back(reference); }
}

template<typename Array>
static void visitReferenceArray(HashSet<ObjectImpl*>& unreferencedObjects,
								std::vector<Object*>& pendingScanObjects,
								const Array& array)
{
	for(auto reference : array)
	{ visitReference(unreferencedObjects, pendingScanObjects, reference); }
}

void Runtime::collectGarbage()
{
	GCGlobals& gcGlobals = GCGlobals::get();
	Lock<Platform::Mutex> lock(gcGlobals.mutex);
	Timing::Timer timer;

	HashSet<ObjectImpl*> unreferencedObjects = gcGlobals.allObjects;
	std::vector<Object*> pendingScanObjects;

	// Initialize the referencedObjects set from the rooted object set.
	Uptr numRoots = 0;
	for(ObjectImpl* object : gcGlobals.allObjects)
	{
		if(object->numRootReferences > 0)
		{
			unreferencedObjects.removeOrFail(object);
			pendingScanObjects.push_back(object);
			++numRoots;
		}
	}

	// Scan the objects added to the referenced set so far: gather their child references and
	// recurse.
	while(pendingScanObjects.size())
	{
		Object* scanObject = pendingScanObjects.back();
		pendingScanObjects.pop_back();

		// Gather the child references for this object based on its kind.
		switch(scanObject->kind)
		{
		case ObjectKind::function:
		{
			FunctionInstance* function = asFunction(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, function->moduleInstance);
			break;
		}
		case ObjectKind::table:
		{
			TableInstance* table = asTable(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, table->compartment);

			Lock<Platform::Mutex> resizingLock(table->resizingMutex);
			const Uptr numElements = getTableNumElements(table);
			for(Uptr elementIndex = 0; elementIndex < numElements; ++elementIndex)
			{
				visitReference(
					unreferencedObjects, pendingScanObjects, getTableElement(table, elementIndex));
			}
			break;
		}
		case ObjectKind::memory:
		{
			MemoryInstance* memory = asMemory(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, memory->compartment);
			break;
		}
		case ObjectKind::global:
		{
			GlobalInstance* global = asGlobal(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, global->compartment);
			break;
		}
		case ObjectKind::moduleInstance:
		{
			ModuleInstance* moduleInstance = asModuleInstance(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, moduleInstance->compartment);
			visitReferenceArray(unreferencedObjects, pendingScanObjects, moduleInstance->functions);
			visitReferenceArray(unreferencedObjects, pendingScanObjects, moduleInstance->tables);
			visitReferenceArray(unreferencedObjects, pendingScanObjects, moduleInstance->memories);
			visitReferenceArray(unreferencedObjects, pendingScanObjects, moduleInstance->globals);
			visitReferenceArray(
				unreferencedObjects, pendingScanObjects, moduleInstance->exceptionTypes);

			{
				Lock<Platform::Mutex> passiveTableSegmentLock(
					moduleInstance->passiveTableSegmentsMutex);
				for(const auto& passiveTableSegmentPair : moduleInstance->passiveTableSegments)
				{
					visitReferenceArray(
						unreferencedObjects, pendingScanObjects, *passiveTableSegmentPair.value);
				}
			}

			break;
		}
		case ObjectKind::context:
		{
			Context* context = asContext(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, context->compartment);
			break;
		}
		case ObjectKind::compartment:
		{
			Compartment* compartment = asCompartment(scanObject);
			visitReference(unreferencedObjects, pendingScanObjects, compartment->wavmIntrinsics);
			break;
		}

		case ObjectKind::module:
		case ObjectKind::exceptionTypeInstance: break;

		default: Errors::unreachable();
		};
	};

	// Call finalize on each unreferenced object.
	for(ObjectImpl* object : unreferencedObjects)
	{
		if(unreferencedObjects.contains(object)) { object->finalize(); }
	}

	// Delete each unreferenced object.
	for(ObjectImpl* object : unreferencedObjects)
	{
		gcGlobals.allObjects.removeOrFail(object);
		delete object;
	}

	Log::printf(Log::metrics,
				"Collected garbage in %.2fms: %" PRIuPTR " roots, %" PRIuPTR " objects, %" PRIuPTR
				" garbage\n",
				timer.getMilliseconds(),
				numRoots,
				Uptr(gcGlobals.allObjects.size() + unreferencedObjects.size()),
				Uptr(unreferencedObjects.size()));
}
