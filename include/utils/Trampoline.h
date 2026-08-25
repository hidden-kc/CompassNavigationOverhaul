#pragma once

#if defined(SKSE_SUPPORT_XBYAK)

#include <Psapi.h>
#include <xbyak/xbyak.h>

#include "SKSE/Trampoline.h"

#include "utils/Logger.h"

namespace hooks
{
	template <std::size_t SrcSize>
	class Hook
	{
		static_assert(SrcSize == 5 || SrcSize == 6);

		static constexpr std::size_t getSizeForSrc()
		{
			// Reference: write_5branch() and write_6branch() of Trampoline.h
			if constexpr (SrcSize == 5)
			{
#pragma pack(push, 1)
				// FF /4
				// JMP r/m64
				struct TrampolineAssembly
				{
					// jmp [rip]
					std::uint8_t jmp;	 // 0 - 0xFF
					std::uint8_t modrm;	 // 1 - 0x25
					std::int32_t disp;	 // 2 - 0x00000000
					std::uint64_t addr;	 // 6 - [rip]
				};
#pragma pack(pop)

				return sizeof(TrampolineAssembly);
			}
			else
			{
				return sizeof(std::uintptr_t);
			}
		}

		static constexpr std::size_t getSizeFor(const Xbyak::CodeGenerator& a_dst)
		{
			return getSizeForSrc() + a_dst.getSize();
		}

	public:

		Hook(std::uintptr_t a_src, const Xbyak::CodeGenerator& a_codeGen) :
			size{ getSizeFor(a_codeGen) }, src{ a_src },
			asmCode{ a_codeGen.getCode(), a_codeGen.getCode() + a_codeGen.getSize() }
		{ }

		Hook(std::uintptr_t a_src, std::uintptr_t a_dst) : 
			size{ getSizeForSrc() }, src{ a_src }, dst{ a_dst }
		{ }

		std::size_t getSize() const { return size; }

		std::uintptr_t getSrc() const { return src; }

		std::uintptr_t getDst() const { return dst; }

		void createDst(SKSE::Trampoline* a_allocator)
		{
			if (!asmCode.empty())
			{
				void* buffer = a_allocator->allocate(asmCode.size());
				std::memcpy(buffer, asmCode.data(), asmCode.size());
				dst = reinterpret_cast<std::uintptr_t>(buffer);
			}
		}

	private:

		std::size_t size;
		std::uintptr_t src;
		std::uintptr_t dst = reinterpret_cast<std::uintptr_t>(nullptr);
		std::vector<std::uint8_t> asmCode;
	};

	class Trampoline
	{
	public:

		// False if the underlying SKSE::Trampoline never got a real memory block to write
		// into - CustomTrampoline leaves it this way rather than crashing when it can't find
		// space near the target module. Check this before write_branch()/write_call() on
		// anything built from CustomTrampoline; DefaultTrampoline always allocates
		// successfully (SKSE::AllocTrampoline fails loudly on its own if it can't).
		bool IsValid() const { return !inst->empty(); }

		template <std::size_t SrcSize>
		std::uintptr_t write_branch(Hook<SrcSize>& a_hook)
		{
			if (!a_hook.getDst()) {
				a_hook.createDst(inst);
			}

			return inst->write_branch<SrcSize>(a_hook.getSrc(), a_hook.getDst());
		}

		template <std::size_t SrcSize>
		std::uintptr_t write_call(Hook<SrcSize>& a_hook)
		{
			if (!a_hook.getDst()) {
				a_hook.createDst(inst);
			}

			return inst->write_call<SrcSize>(a_hook.getSrc(), a_hook.getDst());
		}

	protected:

		Trampoline(SKSE::Trampoline& a_trampoline) :
			inst{ &a_trampoline }
		{ }

		Trampoline(const std::string_view& a_name) :
			inst{ new SKSE::Trampoline{ a_name } }, deleteOnDestruct{ true }
		{ }

		~Trampoline() { if (deleteOnDestruct) delete inst; }

		SKSE::Trampoline* inst;

	private:

		bool deleteOnDestruct = false;
	};

	class DefaultTrampoline : public Trampoline
	{
	public:

		DefaultTrampoline(std::size_t a_size, bool a_trySKSEReserve = true) :
			Trampoline{ SKSE::GetTrampoline() }
		{
			SKSE::AllocTrampoline(a_size, a_trySKSEReserve);
		}
	};

	class CustomTrampoline : public Trampoline
	{
	public:

		// Reference: BranchTrampoline::Create() of https://github.com/ianpatt/skse64/blob/master/skse64_common/BranchTrampoline.cpp
		CustomTrampoline(const std::string_view& a_name, void* a_module, std::size_t a_size) :
			Trampoline{ a_name }
		{
			// search backwards from module base
			auto moduleBase = reinterpret_cast<std::uintptr_t>(a_module);
			std::uintptr_t addr = moduleBase;
			std::uintptr_t maxDisplacement = 0x80000000 - (1024 * 1024 * 128);	// largest 32-bit displacement with 128MB scratch space
			std::uintptr_t lowestOKAddress = (moduleBase >= maxDisplacement) ? moduleBase - maxDisplacement : 0;
			addr--;

			void* base = nullptr;

			while (!base)
			{
				MEMORY_BASIC_INFORMATION info;

				if (!VirtualQuery((void*)addr, &info, sizeof(info)))
				{
					break;
				}

				if (info.State == MEM_FREE)
				{
					// free block, big enough?
					if (info.RegionSize >= a_size)
					{
						// try to allocate it
						addr = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize - a_size;

						base = VirtualAlloc(reinterpret_cast<void*>(addr), a_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
					}
				}

				// move back and try again
				if (!base)
				{
					addr = ((uintptr_t)info.BaseAddress) - 1;
				}

				if (addr < lowestOKAddress)
				{
					break;
				}
			}

			// The backward search above can genuinely fail to find a large-enough free block
			// within displacement range of a_module, especially in a heavily-modded process
			// with many DLLs loaded - this isn't hypothetical, it's what actually happened.
			// Calling set_trampoline(nullptr, a_size, ...) anyway used to leave this
			// Trampoline reporting a non-zero capacity with a null backing pointer, so the
			// first write_branch()/write_call() through it would compute a displacement of
			// roughly -(hook site address) and crash with "displacement is out of range"
			// instead of failing here where the actual cause is obvious. Leave the
			// underlying SKSE::Trampoline in its default (empty) state instead, so
			// IsValid() correctly reports false and the caller can skip the hook.
			if (!base)
			{
				logger::error("CustomTrampoline \"{}\": could not find {} free byte(s) within "
							 "displacement range of module {:#x}; leaving this trampoline unusable",
					a_name, a_size, moduleBase);

				return;
			}

			inst->set_trampoline(base, a_size,
				[](void* a_mem, std::size_t)
				{
					SKSE::WinAPI::VirtualFree(a_mem, 0, MEM_RELEASE);
				});
		}
	};

	class SigScanner
	{
	public:

		 template <SKSE::stl::nttp::string str>
		static std::uintptr_t FindPattern(SKSE::WinAPI::HMODULE a_moduleHandle)
		{
			MODULEINFO moduleInfo;
			GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(a_moduleHandle), &moduleInfo, sizeof(MODULEINFO));
			auto base = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
			std::size_t size = moduleInfo.SizeOfImage;

			std::size_t patternLength = (str.length() + 1) / 3; // 2/3 useful chars (1 space between byte chars)
			auto pattern = REL::make_pattern<str>();

			for (std::size_t offset = 0; offset < size - patternLength; offset++)
			{
				std::uintptr_t addr = base + offset;

				if (pattern.match(addr))
				{
					return addr;
				}
			}

			return reinterpret_cast<std::uintptr_t>(nullptr);
		}
	};
}

#else
#error "Xbyak support needed for Trampoline"
#endif