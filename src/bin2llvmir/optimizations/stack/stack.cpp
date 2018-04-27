/**
* @file src/bin2llvmir/optimizations/stack/stack.cpp
* @brief Reconstruct stack.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#define debug_enabled false
#include "retdec/bin2llvmir/utils/utils.h"
#include "retdec/bin2llvmir/utils/type.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char StackAnalysis::ID = 0;

static RegisterPass<StackAnalysis> X(
		"stack",
		"Stack optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

StackAnalysis::StackAnalysis() :
		ModulePass(ID)
{

}

bool StackAnalysis::runOnModule(llvm::Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_dbgf = DebugFormatProvider::getDebugFormat(_module);
	return run();
}

bool StackAnalysis::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		DebugFormat* dbgf)
{
	_module = &m;
	_config = c;
	_dbgf = dbgf;
	return run();
}

bool StackAnalysis::run()
{
	if (_config == nullptr)
	{
		return false;
	}

	bool changed = false;

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*_module, _config);

	for (auto& f : *_module)
	{
		changed |= runOnFunction(RDA, &f);
	}

	return changed;
}

bool StackAnalysis::runOnFunction(
		ReachingDefinitionsAnalysis& RDA,
		llvm::Function* f)
{
	bool changed = false;

	LOG << "\tfunction : " << f->getName().str() << std::endl;

	std::map<Value*, Value*> val2val;
	std::list<ReplaceItem> replaceItems;

	for (auto &bb : *f)
	for (auto &i : bb)
	{
		if (StoreInst *store = dyn_cast<StoreInst>(&i))
		{
			if (AsmInstruction::isLlvmToAsmInstruction(store))
			{
				continue;
			}

			handleInstruction(
					RDA,
					store,
					store->getValueOperand(),
					store->getValueOperand()->getType(),
					replaceItems,
					val2val);
		}
	}

	for (auto &bb : *f)
	for (auto &i : bb)
	{
		if (LoadInst* load = dyn_cast<LoadInst>(&i))
		{
			auto* pt = load->getPointerOperand()->getType()->getPointerElementType();
			if (pt && pt->isIntegerTy(1))
			{
				continue;
			}

			if (isa<GlobalVariable>(load->getPointerOperand()))
			{
				continue;
			}

			changed |= handleInstruction(
					RDA,
					load,
					load->getPointerOperand(),
					load->getType(),
					replaceItems,
					val2val);
		}
		else if (StoreInst *store = dyn_cast<StoreInst>(&i))
		{
			if (AsmInstruction::isLlvmToAsmInstruction(store))
			{
				continue;
			}

			auto* pt = store->getPointerOperand()->getType()->getPointerElementType();
			if (pt && pt->isIntegerTy(1))
			{
				continue;
			}

			if (!isa<GlobalVariable>(store->getPointerOperand()))
			{
				changed |= handleInstruction(
						RDA,
						store,
						store->getPointerOperand(),
						store->getValueOperand()->getType(),
						replaceItems,
						val2val);
			}
		}
	}

	std::set<Instruction*> toErase;
	for (auto& ri : replaceItems)
	{
		auto* s = dyn_cast<StoreInst>(ri.inst);
		auto* l = dyn_cast<LoadInst>(ri.inst);
		if (s && s->getPointerOperand() == ri.from)
		{
			// TODO: if would be better, it else branch here was not needed.
			// We would only replace load/store pointer operand and the type
			// propagation would be handled later by som other related analysis.
			//
			if (ri.to->getAllocatedType()->isAggregateType())
			{
				auto* conv = convertValueToType(
						ri.to,
						s->getPointerOperand()->getType(),
						ri.inst);
				s->setOperand(s->getPointerOperandIndex(), conv);
			}
			else
			{
				auto* conv = convertValueToType(
						s->getValueOperand(),
						ri.to->getType()->getElementType(),
						ri.inst);
				new StoreInst(conv, ri.to, ri.inst);
				toErase.insert(s);
				new StoreInst(conv, ri.to, ri.inst);
				toErase.insert(s);
			}
		}
		else if (l && l->getPointerOperand() == ri.from)
		{
			if (ri.to->getAllocatedType()->isAggregateType())
			{
				auto* conv = convertValueToType(
						ri.to,
						l->getPointerOperand()->getType(),
						ri.inst);
				l->setOperand(l->getPointerOperandIndex(), conv);
			}
			else
			{
				auto* nl = new LoadInst(ri.to, "", l);
				auto* conv = convertValueToType(nl, l->getType(), l);
				l->replaceAllUsesWith(conv);
				toErase.insert(l);
			}
		}
		else
		{
			auto* conv = convertValueToType(ri.to, ri.from->getType(), ri.inst);
			ri.inst->replaceUsesOfWith(ri.from, conv);
		}
	}
	for (auto* e : toErase)
	{
		e->eraseFromParent();
	}

	return changed;
}

bool StackAnalysis::handleInstruction(
		ReachingDefinitionsAnalysis& RDA,
		llvm::Instruction* inst,
		llvm::Value* val,
		llvm::Type* type,
		std::list<ReplaceItem>& replaceItems,
		std::map<llvm::Value*, llvm::Value*>& val2val)
{
	LOG << "@ " << AsmInstruction::getInstructionAddress(inst)
			<< " -- " << llvmObjToString(inst) << std::endl;

	SymbolicTree root(RDA, val, &val2val);
	LOG << root << std::endl;

	if (!root.isVal2ValMapUsed())
	{
		bool stackPtr = false;
		for (SymbolicTree* n : root.getPostOrder())
		{
			if (_config->isStackPointerRegister(n->value))
			{
				stackPtr = true;
				break;
			}
		}
		if (!stackPtr)
		{
			LOG << "===> no SP" << std::endl;
			return false;
		}
	}

	auto* debugSv = getDebugStackVariable(inst->getFunction(), root);

	root.simplifyNode(_config);
	LOG << root << std::endl;

	if (debugSv == nullptr)
	{
		debugSv = getDebugStackVariable(inst->getFunction(), root);
	}

	auto* ci = dyn_cast_or_null<ConstantInt>(root.value);
	if (ci == nullptr)
	{
		return false;
	}

	if (auto* s = dyn_cast<StoreInst>(inst))
	{
		if (s->getValueOperand() == val)
		{
			val2val[inst] = ci;
		}
	}

	LOG << "===> " << llvmObjToString(ci) << std::endl;
	LOG << "===> " << ci->getSExtValue() << std::endl;

	std::string name = debugSv ? debugSv->getName() : "";
	Type* t = debugSv ?
			stringToLlvmTypeDefault(_module, debugSv->type.getLlvmIr()) :
			type;

	IrModifier irModif(_module, _config);
	auto p = irModif.getStackVariable(
			inst->getFunction(),
			ci->getSExtValue(),
			t,
			name);

	AllocaInst* a = p.first;
	auto* ca = p.second;

	if (debugSv)
	{
		ca->setIsFromDebug(true);
		ca->setRealName(debugSv->getName());
	}

	replaceItems.push_back(ReplaceItem{inst, val, a});

	LOG << "===> " << llvmObjToString(a) << std::endl;
	LOG << "===> " << llvmObjToString(inst) << std::endl;
	LOG << std::endl;

	return true;
}

/**
 * Find a value that is being added to the stack pointer register in \p root.
 * Find a debug variable with offset equal to this value.
 */
retdec::config::Object* StackAnalysis::getDebugStackVariable(
		llvm::Function* fnc,
		SymbolicTree& root)
{
	if (_dbgf == nullptr)
	{
		return nullptr;
	}
	auto* debugFnc = _dbgf->getFunction(_config->getFunctionAddress(fnc));
	if (debugFnc == nullptr)
	{
		return nullptr;
	}

	retdec::utils::Maybe<int> baseOffset;
	if (auto* ci = dyn_cast_or_null<ConstantInt>(root.value))
	{
		baseOffset = ci->getSExtValue();
	}
	else
	{
		for (SymbolicTree* n : root.getLevelOrder())
		{
			if (isa<AddOperator>(n->value)
					&& n->ops.size() == 2
					&& isa<LoadInst>(n->ops[0].value)
					&& isa<ConstantInt>(n->ops[1].value))
			{
				auto* l = cast<LoadInst>(n->ops[0].value);
				auto* ci = cast<ConstantInt>(n->ops[1].value);
				if (_config->isRegister(l->getPointerOperand()))
				{
					baseOffset = ci->getSExtValue();
				}
				break;
			}
		}
	}
	if (baseOffset.isUndefined())
	{
		return nullptr;
	}

	for (auto& p : debugFnc->locals)
	{
		auto& var = p.second;
		if (!var.getStorage().isStack())
		{
			continue;
		}
		if (var.getStorage().getStackOffset() == baseOffset)
		{
			return &var;
		}
	}

	return nullptr;
}

} // namespace bin2llvmir
} // namespace retdec
