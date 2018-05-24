/**
 * @file include/retdec/bin2llvmir/providers/abi/abi.h
 * @brief ABI information.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_ABI_ABI_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_ABI_ABI_H

#include <map>
#include <memory>
#include <set>
#include <vector>

#include <llvm/IR/Module.h>

#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/providers/config.h"

//#include "retdec/capstone2llvmir/x86/x86_defs.h"

namespace retdec {
namespace bin2llvmir {

class Abi
{
	// Constants.
	//
	public:
		static const uint32_t REG_INVALID;

	// Ctors, dtors.
	//
	public:
		Abi(llvm::Module* m, Config* c);
		virtual ~Abi();

	// Registers.
	//
	public:
		bool isRegister(const llvm::Value* val);
		bool isFlagRegister(const llvm::Value* val);
		bool isStackPointerRegister(const llvm::Value* val);

		llvm::GlobalVariable* getRegister(uint32_t r);
		uint32_t getRegisterId(llvm::Value* r);
		const std::vector<llvm::GlobalVariable*>& getRegisters() const;

		void addRegister(uint32_t id, llvm::GlobalVariable* reg);

	// Instructions.
	//
	public:
		bool isNopInstruction(AsmInstruction ai);
		virtual bool isNopInstruction(cs_insn* insn) = 0;

	// Types.
	//
	public:
		std::size_t getTypeByteSize(llvm::Type* t);
		std::size_t getTypeBitSize(llvm::Type* t);

	// Private data - misc.
	//
	protected:
		llvm::Module* _module = nullptr;
		Config* _config = nullptr;

	// Private data - registers.
	//
	protected:
		/// Fast iteration over all registers.
		/// \c id2regs may contain \c nullptr values.
		std::vector<llvm::GlobalVariable*> _regs;
		/// Fast "capstone id -> LLVM value" search.
		std::vector<llvm::GlobalVariable*> _id2regs;
		/// Fast "is LLVM value a register?" check.
		/// Fast "LLVM value -> capstone id" search.
		std::map<const llvm::Value*, uint32_t> _regs2id;
		/// ID of stack pointer register.
		uint32_t _regStackPointerId = REG_INVALID;
};

class AbiProvider
{
	public:
		static Abi* addAbi(
				llvm::Module* m,
				Config* c);
		static Abi* getAbi(llvm::Module* m);
		static bool getAbi(llvm::Module* m, Abi*& abi);
		static void clear();

	private:
		static std::map<llvm::Module*, std::unique_ptr<Abi>> _module2abi;
};

} // namespace bin2llvmir
} // namespace retdec

#endif