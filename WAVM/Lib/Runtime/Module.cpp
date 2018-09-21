#include <string.h>
#include <atomic>
#include <memory>
#include <utility>

#include "RuntimePrivate.h"
#include "WAVM/IR/IR.h"
#include "WAVM/IR/Module.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Hash.h"
#include "WAVM/Inline/HashMap.h"
#include "WAVM/Inline/Lock.h"
#include "WAVM/Inline/Serialization.h"
#include "WAVM/LLVMJIT/LLVMJIT.h"
#include "WAVM/Platform/Intrinsic.h"
#include "WAVM/Platform/Mutex.h"
#include "WAVM/Runtime/Runtime.h"

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;

static Value evaluateInitializer(const std::vector<GlobalInstance*>& moduleGlobals,
								 InitializerExpression expression)
{
	switch(expression.type)
	{
	case InitializerExpression::Type::i32_const: return expression.i32;
	case InitializerExpression::Type::i64_const: return expression.i64;
	case InitializerExpression::Type::f32_const: return expression.f32;
	case InitializerExpression::Type::f64_const: return expression.f64;
	case InitializerExpression::Type::v128_const: return expression.v128;
	case InitializerExpression::Type::get_global:
	{
		// Find the import this refers to.
		errorUnless(expression.globalIndex < moduleGlobals.size());
		GlobalInstance* globalInstance = moduleGlobals[expression.globalIndex];
		errorUnless(globalInstance);
		errorUnless(!globalInstance->type.isMutable);
		return IR::Value(globalInstance->type.valueType, globalInstance->initialValue);
	}
	case InitializerExpression::Type::ref_null: return nullptr;
	default: Errors::unreachable();
	};
}

Runtime::Module* Runtime::compileModule(const IR::Module& irModule)
{
	std::vector<U8> objectCode = LLVMJIT::compileModule(irModule);
	return new Module(IR::Module(irModule), std::move(objectCode));
}

std::vector<U8> Runtime::getObjectCode(Runtime::Module* module) { return module->objectCode; }

Runtime::Module* Runtime::loadPrecompiledModule(const IR::Module& irModule,
												const std::vector<U8>& objectCode)
{
	return new Module(IR::Module(irModule), std::vector<U8>(objectCode));
}

ModuleInstance::~ModuleInstance()
{
	if(jitModule)
	{
		LLVMJIT::unloadModule(jitModule);
		jitModule = nullptr;
	}
}

ModuleInstance* Runtime::instantiateModule(Compartment* compartment,
										   Module* module,
										   ImportBindings&& imports,
										   std::string&& moduleDebugName)
{
	// Create the ModuleInstance and add it to the compartment's modules list.
	ModuleInstance* moduleInstance = new ModuleInstance(compartment,
														std::move(imports.functions),
														std::move(imports.tables),
														std::move(imports.memories),
														std::move(imports.globals),
														std::move(imports.exceptionTypes),
														std::move(moduleDebugName));
	{
		Lock<Platform::Mutex> compartmentLock(compartment->mutex);
		compartment->modules.addOrFail(moduleInstance);
	}

	// Check the type of the ModuleInstance's imports.
	errorUnless(moduleInstance->functions.size() == module->ir.functions.imports.size());
	for(Uptr importIndex = 0; importIndex < module->ir.functions.imports.size(); ++importIndex)
	{
		errorUnless(isA(moduleInstance->functions[importIndex],
						module->ir.types[module->ir.functions.imports[importIndex].type.index]));
	}
	errorUnless(moduleInstance->tables.size() == module->ir.tables.imports.size());
	for(Uptr importIndex = 0; importIndex < module->ir.tables.imports.size(); ++importIndex)
	{
		errorUnless(
			isA(moduleInstance->tables[importIndex], module->ir.tables.imports[importIndex].type));
	}
	errorUnless(moduleInstance->memories.size() == module->ir.memories.imports.size());
	for(Uptr importIndex = 0; importIndex < module->ir.memories.imports.size(); ++importIndex)
	{
		errorUnless(isA(moduleInstance->memories[importIndex],
						module->ir.memories.imports[importIndex].type));
	}
	errorUnless(moduleInstance->globals.size() == module->ir.globals.imports.size());
	for(Uptr importIndex = 0; importIndex < module->ir.globals.imports.size(); ++importIndex)
	{
		errorUnless(isA(moduleInstance->globals[importIndex],
						module->ir.globals.imports[importIndex].type));
	}
	errorUnless(moduleInstance->exceptionTypes.size() == module->ir.exceptionTypes.imports.size());
	for(Uptr importIndex = 0; importIndex < module->ir.exceptionTypes.imports.size(); ++importIndex)
	{
		errorUnless(isA(moduleInstance->exceptionTypes[importIndex],
						module->ir.exceptionTypes.imports[importIndex].type));
	}

	// Deserialize the disassembly names.
	DisassemblyNames disassemblyNames;
	getDisassemblyNames(module->ir, disassemblyNames);

	// Instantiate the module's memory and table definitions.
	for(Uptr tableDefIndex = 0; tableDefIndex < module->ir.tables.defs.size(); ++tableDefIndex)
	{
		std::string debugName
			= disassemblyNames.tables[module->ir.tables.imports.size() + tableDefIndex];
		auto table = createTable(
			compartment, module->ir.tables.defs[tableDefIndex].type, std::move(debugName));
		if(!table) { throwException(Exception::outOfMemoryType); }
		moduleInstance->tables.push_back(table);
	}
	for(Uptr memoryDefIndex = 0; memoryDefIndex < module->ir.memories.defs.size(); ++memoryDefIndex)
	{
		std::string debugName
			= disassemblyNames.memories[module->ir.memories.imports.size() + memoryDefIndex];
		auto memory = createMemory(
			compartment, module->ir.memories.defs[memoryDefIndex].type, std::move(debugName));
		if(!memory) { throwException(Exception::outOfMemoryType); }
		moduleInstance->memories.push_back(memory);
	}

	// Find the default memory and table for the module and initialize the runtime data memory/table
	// base pointers.
	if(moduleInstance->memories.size() != 0)
	{
		wavmAssert(moduleInstance->memories.size() == 1);
		moduleInstance->defaultMemory = moduleInstance->memories[0];
	}
	if(moduleInstance->tables.size() != 0)
	{ moduleInstance->defaultTable = moduleInstance->tables[0]; }

	// Instantiate the module's global definitions.
	for(const GlobalDef& globalDef : module->ir.globals.defs)
	{
		const Value initialValue
			= evaluateInitializer(moduleInstance->globals, globalDef.initializer);
		errorUnless(isSubtype(initialValue.type, globalDef.type.valueType));
		moduleInstance->globals.push_back(createGlobal(compartment, globalDef.type, initialValue));
	}

	// Instantiate the module's exception types.
	for(const ExceptionTypeDef& exceptionTypeDef : module->ir.exceptionTypes.defs)
	{
		moduleInstance->exceptionTypes.push_back(
			createExceptionTypeInstance(exceptionTypeDef.type, "wasmException"));
	}

	// Instantiate the module's defined functions.
	for(Uptr functionDefIndex = 0; functionDefIndex < module->ir.functions.defs.size();
		++functionDefIndex)
	{
		const DisassemblyNames::Function& functionNames
			= disassemblyNames.functions[module->ir.functions.imports.size() + functionDefIndex];
		std::string debugName = functionNames.name;
		if(!debugName.size())
		{ debugName = "<function #" + std::to_string(functionDefIndex) + ">"; }

		auto functionInstance = new FunctionInstance(
			moduleInstance,
			module->ir.types[module->ir.functions.defs[functionDefIndex].type.index],
			nullptr,
			IR::CallingConvention::wasm,
			std::move(debugName));
		moduleInstance->functionDefs.push_back(functionInstance);
		moduleInstance->functions.push_back(functionInstance);
	}

	HashMap<std::string, LLVMJIT::FunctionBinding> wavmIntrinsicsExportMap;
	for(auto intrinsicExport : compartment->wavmIntrinsics->exportMap)
	{
		FunctionInstance* intrinsicFunction = asFunction(intrinsicExport.value);
		wavmAssert(intrinsicFunction);
		wavmAssert(intrinsicFunction->callingConvention == IR::CallingConvention::intrinsic);
		errorUnless(wavmIntrinsicsExportMap.add(
			intrinsicExport.key, LLVMJIT::FunctionBinding{intrinsicFunction->nativeFunction}));
	}

	std::vector<FunctionType> jitTypes = module->ir.types;
	LLVMJIT::MemoryBinding jitDefaultMemory{
		moduleInstance->defaultMemory ? moduleInstance->defaultMemory->id : UINTPTR_MAX};
	LLVMJIT::TableBinding jitDefaultTable{
		moduleInstance->defaultTable ? moduleInstance->defaultTable->id : UINTPTR_MAX};

	std::vector<LLVMJIT::FunctionBinding> jitFunctionImports;
	for(Uptr importIndex = 0; importIndex < module->ir.functions.imports.size(); ++importIndex)
	{
		FunctionInstance* functionImport = moduleInstance->functions[importIndex];
		void* nativeFunction = functionImport->nativeFunction;
		if(functionImport->callingConvention != IR::CallingConvention::wasm)
		{
			nativeFunction = LLVMJIT::getIntrinsicThunk(nativeFunction,
														functionImport,
														functionImport->type,
														functionImport->callingConvention);
		}
		jitFunctionImports.push_back({nativeFunction});
	}

	std::vector<LLVMJIT::TableBinding> jitTables;
	for(TableInstance* table : moduleInstance->tables) { jitTables.push_back({table->id}); }

	std::vector<LLVMJIT::MemoryBinding> jitMemories;
	for(MemoryInstance* memory : moduleInstance->memories) { jitMemories.push_back({memory->id}); }

	std::vector<LLVMJIT::GlobalBinding> jitGlobals;
	for(GlobalInstance* global : moduleInstance->globals)
	{
		LLVMJIT::GlobalBinding globalSpec;
		globalSpec.type = global->type;
		if(global->type.isMutable) { globalSpec.mutableGlobalId = global->mutableGlobalId; }
		else
		{
			globalSpec.immutableValuePointer = &global->initialValue;
		}
		jitGlobals.push_back(globalSpec);
	}

	std::vector<ExceptionTypeInstance*> jitExceptionTypes;
	for(ExceptionTypeInstance* exceptionTypeInstance : moduleInstance->exceptionTypes)
	{ jitExceptionTypes.push_back(exceptionTypeInstance); }

	// Load the compiled module's object code with this module instance's imports.
	std::vector<LLVMJIT::JITFunction*> jitFunctionDefs;
	moduleInstance->jitModule = LLVMJIT::loadModule(module->objectCode,
													std::move(wavmIntrinsicsExportMap),
													std::move(jitTypes),
													std::move(jitFunctionImports),
													std::move(jitTables),
													std::move(jitMemories),
													std::move(jitGlobals),
													std::move(jitExceptionTypes),
													jitDefaultMemory,
													jitDefaultTable,
													moduleInstance,
													reinterpret_cast<Uptr>(getOutOfBoundsAnyFunc()),
													moduleInstance->functionDefs,
													jitFunctionDefs);

	// Link the JITFunction to the corresponding FunctionInstance, and the FunctionInstance to the
	// compiled machine code.
	for(Uptr functionDefIndex = 0; functionDefIndex < module->ir.functions.defs.size();
		++functionDefIndex)
	{
		LLVMJIT::JITFunction* jitFunction = jitFunctionDefs[functionDefIndex];
		FunctionInstance* functionInstance = moduleInstance->functionDefs[functionDefIndex];
		functionInstance->nativeFunction = reinterpret_cast<void*>(jitFunction->baseAddress);
		jitFunction->type = LLVMJIT::JITFunction::Type::wasmFunction;
		jitFunction->functionInstance = functionInstance;
	}

	// Set up the instance's exports.
	for(const Export& exportIt : module->ir.exports)
	{
		Object* exportedObject = nullptr;
		switch(exportIt.kind)
		{
		case IR::ObjectKind::function:
			exportedObject = moduleInstance->functions[exportIt.index];
			break;
		case IR::ObjectKind::table: exportedObject = moduleInstance->tables[exportIt.index]; break;
		case IR::ObjectKind::memory:
			exportedObject = moduleInstance->memories[exportIt.index];
			break;
		case IR::ObjectKind::global:
			exportedObject = moduleInstance->globals[exportIt.index];
			break;
		case IR::ObjectKind::exceptionType:
			exportedObject = moduleInstance->exceptionTypes[exportIt.index];
			break;
		default: Errors::unreachable();
		}
		moduleInstance->exportMap.addOrFail(exportIt.name, exportedObject);
	}

	// Copy the module's data segments into the module's default memory.
	for(const DataSegment& dataSegment : module->ir.dataSegments)
	{
		if(dataSegment.isActive)
		{
			MemoryInstance* memory = moduleInstance->memories[dataSegment.memoryIndex];

			const Value baseOffsetValue
				= evaluateInitializer(moduleInstance->globals, dataSegment.baseOffset);
			errorUnless(baseOffsetValue.type == ValueType::i32);
			const U32 baseOffset = baseOffsetValue.i32;

			if(dataSegment.data.size())
			{
				Platform::bytewiseMemCopy(memory->baseAddress + baseOffset,
										  dataSegment.data.data(),
										  dataSegment.data.size());
			}
			else
			{
				// WebAssembly still expects out-of-bounds errors if the segment base offset is
				// out-of-bounds, even if the segment is empty.
				if(baseOffset > memory->numPages * IR::numBytesPerPage)
				{
					throwException(Runtime::Exception::outOfBoundsMemoryAccessType,
								   {asAnyRef(memory), U64(baseOffset)});
				}
			}
		}
	}

	// Copy the module's table segments into the module's default table.
	for(const TableSegment& tableSegment : module->ir.tableSegments)
	{
		if(tableSegment.isActive)
		{
			TableInstance* table = moduleInstance->tables[tableSegment.tableIndex];

			const Value baseOffsetValue
				= evaluateInitializer(moduleInstance->globals, tableSegment.baseOffset);
			errorUnless(baseOffsetValue.type == ValueType::i32);
			const U32 baseOffset = baseOffsetValue.i32;

			if(tableSegment.indices.size())
			{
				for(Uptr index = 0; index < tableSegment.indices.size(); ++index)
				{
					const Uptr functionIndex = tableSegment.indices[index];
					wavmAssert(functionIndex < moduleInstance->functions.size());
					const AnyFunc* anyFunc = asAnyFunc(moduleInstance->functions[functionIndex]);
					setTableElement(table, baseOffset + index, &anyFunc->anyRef);
				}
			}
			else
			{
				// WebAssembly still expects out-of-bounds errors if the segment base offset is
				// out-of-bounds, even if the segment is empty.
				if(baseOffset > getTableNumElements(table))
				{
					throwException(Runtime::Exception::outOfBoundsTableAccessType,
								   {asAnyRef(table), U64(baseOffset)});
				}
			}
		}
	}

	// Copy the module's passive data and table segments into the ModuleInstance for later use.
	for(Uptr segmentIndex = 0; segmentIndex < module->ir.dataSegments.size(); ++segmentIndex)
	{
		const DataSegment& dataSegment = module->ir.dataSegments[segmentIndex];
		if(!dataSegment.isActive)
		{
			moduleInstance->passiveDataSegments.add(
				segmentIndex, std::make_shared<std::vector<U8>>(dataSegment.data));
		}
	}
	for(Uptr segmentIndex = 0; segmentIndex < module->ir.tableSegments.size(); ++segmentIndex)
	{
		const TableSegment& tableSegment = module->ir.tableSegments[segmentIndex];
		if(!tableSegment.isActive)
		{
			auto passiveTableSegmentObjects = std::make_shared<std::vector<Object*>>();
			for(Uptr functionIndex : tableSegment.indices)
			{ passiveTableSegmentObjects->push_back(moduleInstance->functions[functionIndex]); }
			moduleInstance->passiveTableSegments.add(segmentIndex, passiveTableSegmentObjects);
		}
	}

	// Look up the module's start function.
	if(module->ir.startFunctionIndex != UINTPTR_MAX)
	{
		moduleInstance->startFunction = moduleInstance->functions[module->ir.startFunctionIndex];
		wavmAssert(moduleInstance->startFunction->type == IR::FunctionType());
	}

	return moduleInstance;
}

void ModuleInstance::finalize()
{
	Lock<Platform::Mutex> compartmentLock(compartment->mutex);
	compartment->modules.removeOrFail(this);
}

FunctionInstance* Runtime::getStartFunction(ModuleInstance* moduleInstance)
{
	return moduleInstance->startFunction;
}

MemoryInstance* Runtime::getDefaultMemory(ModuleInstance* moduleInstance)
{
	return moduleInstance->defaultMemory;
}
TableInstance* Runtime::getDefaultTable(ModuleInstance* moduleInstance)
{
	return moduleInstance->defaultTable;
}

Object* Runtime::getInstanceExport(ModuleInstance* moduleInstance, const std::string& name)
{
	wavmAssert(moduleInstance);
	Object* const* exportedObjectPtr = moduleInstance->exportMap.get(name);
	return exportedObjectPtr ? *exportedObjectPtr : nullptr;
}
