#include "PeLibInclude.h" 

#include "PeRecompiler.h"
#include "VectorUtils.h"
#include "RewriteBlock.h"
#include "ASLRPreselectionStub.h"

#include <algorithm>
#include <functional>

#include <Windows.h>


const uint32_t TRICKY_BASE_ADDRESS = 0xFFFF0000;
const uint32_t ACTUALIZED_BASE_ADDRESS = 0x00010000;

PeSectionContents::PeSectionContents(uint32_t index, std::shared_ptr<PeLib::PeFile32> &_header, std::ifstream &file)
{
	auto& peHeader = _header->peHeader();
	this->index = index;
	this->RVA = peHeader.getVirtualAddress(index);
	this->size = peHeader.getSizeOfRawData(index);
	this->rawPointer = peHeader.getPointerToRawData(index);
	this->virtualSize = peHeader.getVirtualSize(index);
	this->name = peHeader.getSectionName(index);

	char* block = new char[this->size];
	file.seekg(this->rawPointer, std::ios::beg);
	file.read(block, this->size);

	this->data = std::vector<uint8_t>(block, block + this->size);
	delete[] block;
}

void PeSectionContents::print(std::ostream &stream)
{
	auto writePadHex = [&stream](uint32_t val) -> void
	{
		stream << "0x" << std::hex << std::left << std::setfill('0') << std::setw(8) << val << "  ";
	};

	stream << "\t";
	stream << std::left << std::setfill(' ') << std::setw(10) << this->name;
	writePadHex(this->virtualSize);
	writePadHex(this->size);
	writePadHex(this->RVA);
	writePadHex(this->rawPointer);
	stream << std::endl;
}


template <class RWBLOCK, typename... ARGS>
void PeRecompiler::addRewriteBlock(ARGS... args)
{
	auto block = std::shared_ptr<RewriteBlock>(new RWBLOCK(args...));
	this->rewriteBlocks.push_back(block);

	if (this->multiPass)
	{
		auto count = 0;
		auto newptr = block;
		while (true)
		{
			newptr = newptr->getNextMultiPassBlock(count++);
			if (newptr.get() != nullptr)
				this->rewriteBlocks.push_back(newptr);
			else
				break;
		}
	}
}


PeRecompiler::PeRecompiler(
	std::ostream &_infoStream, std::ostream &_errorStream,
	const std::string &_inputFileName, const std::string &_outputFileName
)
	: infoStream(_infoStream), errorStream(_errorStream),
	inputFileName(_inputFileName), outputFileName(_outputFileName),
	multiPass(false), shouldUseWin10Attack(false)
{
	this->infoStream << std::hex;
	this->errorStream << std::hex;
}


void PeRecompiler::useWindows10Attack(bool win10)
{
	this->shouldUseWin10Attack = win10;
}

void PeRecompiler::doMultiPass(bool multi)
{
	this->multiPass = multi;
}

bool PeRecompiler::loadInputFile()
{
	auto peFile = std::make_shared<PeLib::PeFile32>(this->inputFileName);
	if (peFile->readMzHeader() != NO_ERROR)
	{
		this->errorStream << "Failed to read MzHeader: " << this->inputFileName << std::endl;
		return false;
	}

	if (peFile->readPeHeader() != NO_ERROR)
	{
		this->errorStream << "Failed to read PeHeader: " << this->inputFileName << std::endl;
		return false;
	}

	this->peFile = peFile;
	this->infoStream << "Successfully loaded PE File: " << this->inputFileName << std::endl;
	return true;
}

bool PeRecompiler::loadInputSections()
{
	if (!this->peFile)
		return false;

	std::ifstream file(this->inputFileName.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		this->errorStream << "Failed to open original file for section reading: " << this->inputFileName << std::endl;
		return false;
	}

	auto& peHeader = this->peFile->peHeader();
	this->infoStream << "Loading sections" << std::endl;
	this->infoStream << "\t";
	this->infoStream << std::left << std::setfill(' ') << std::setw(10) << "Name";
	this->infoStream << std::hex << std::left << std::setw(12) << "VirtSize";
	this->infoStream << std::hex << std::left << std::setw(12) << "RawSize";
	this->infoStream << std::hex << std::left << std::setw(12) << "VirtAddr";
	this->infoStream << std::hex << std::left << std::setw(12) << "RawAddr";
	this->infoStream << std::endl;

	for (unsigned int sec = 0; sec < peHeader.getNumberOfSections(); sec++)
	{
		auto sc = std::make_shared<PeSectionContents>(sec, this->peFile, file);
		sc->print(this->infoStream);
		this->sectionContents.push_back(sc);
	}

	file.close();

	/* 
	TODO:
		when allocSection() has been properly finished and is able to serve
		a new relocations section during building, we can remove the following
		two checks; the first is done later as well, and the second won't be
		needed once section alloc is properly working.
	*/
	auto relocSec = this->getSectionByRVA(peHeader.getIddBaseRelocRva(), 4);
	if (!relocSec)
	{
		this->errorStream << "Failed to locate reloc section!" << std::endl;
		return false;
	}
	if (relocSec->index != (peHeader.getNumberOfSections() - 1))
	{
		this->errorStream << "Reloc section '" << relocSec->name << "' is not final section; currently unsupported" << std::endl;
		return false;
	}

	return true;
}

bool PeRecompiler::performOnDiskRelocations()
{
	if (!this->peFile)
		return false;

	/* first, make sure everything is as it must be */
	if (!this->sectionContents.size())
	{
		this->errorStream << "Section contents must be loaded before doing any relocations!" << std::endl;
		return false;
	}

	auto& peHeader = this->peFile->peHeader();

	uint32_t characteristics = peHeader.getDllCharacteristics();
	uint32_t requestedBase = peHeader.getImageBase();

	if ((characteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)
	{
		this->errorStream << "Binary must have ASLR enabled to perform on-disk relocations!" << std::endl;
		return false;
	}

	if (this->peFile->readRelocationsDirectory())
	{
		this->errorStream << "Failed to read reloc directory!" << std::endl;
		return false;
	}

	this->infoStream << "Preparing header for obfuscation" << std::endl;

	/*
		now, our first step is to remove the ASLR flag and request a base of TRICKY_BASE_ADDRESS.
		this actually causes us to load at ACTUALIZED_BASE_ADDRESS, though, so we will relocate to there.
	*/
	if (!this->shouldUseWin10Attack)
	{
		auto newCharacteristics = (characteristics & ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
		peHeader.setDllCharacteristics(newCharacteristics);
		this->infoStream << "\tStripped IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE flag" << std::endl;
		this->infoStream << "\t\tOld Characteristics: 0x" << characteristics << std::endl;
		this->infoStream << "\t\tNew Characteristics: 0x" << newCharacteristics << std::endl;
	}
	else if (characteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)
	{
		this->infoStream << "\t[Win10 Attack] Leaving IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE set" << std::endl;
	}
	else
	{
		auto newCharacteristics = (characteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
		peHeader.setDllCharacteristics(newCharacteristics);
		this->infoStream << "\t[Win10 Attack] Added IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE flag" << std::endl;
		this->infoStream << "\t\tOld Characteristics: 0x" << characteristics << std::endl;
		this->infoStream << "\t\tNew Characteristics: 0x" << newCharacteristics << std::endl;
	}


	if (!this->shouldUseWin10Attack)
	{
		peHeader.setImageBase(TRICKY_BASE_ADDRESS);
		this->infoStream << "\tChanged ImageBase to 0x" << TRICKY_BASE_ADDRESS << " (was 0x" << requestedBase << ")" << std::endl;
	}
	else
	{
		this->infoStream << "\t[Win10 Attack] Leaving ImageBase as 0x" << requestedBase << std::endl;
	}



	/* now let's relocate everything to 0x00010000 */
	uint32_t numberOfRelocsPerformed = 0;
	auto& reloc = this->peFile->relocDir();
	const int32_t relocDelta = ACTUALIZED_BASE_ADDRESS - requestedBase;
	for (unsigned int rel = 0; rel < reloc.calcNumberOfRelocations(); rel++)
	{
		uint32_t relocBlockRVA = reloc.getVirtualAddress(rel);
		uint32_t relocBlockSize = reloc.getSizeOfBlock(rel);
		auto sc = this->getSectionByRVA(relocBlockRVA, 4);
		if (!sc)
		{
			this->errorStream << "Reloc has no matching section! RVA: 0x" << relocBlockRVA << std::endl;
			return false;
		}

		auto relocBlockCount = reloc.calcNumberOfRelocationData(rel);
		for (unsigned int relEntry = 0; relEntry < relocBlockCount; relEntry++)
		{
			uint16_t entry = reloc.getRelocationData(rel, relEntry);
			uint16_t entryType = (entry >> 12);
			uint32_t entryAddress = relocBlockRVA + (entry & 0x0FFF);

			uint32_t si = entryAddress - sc->RVA;
			if (entryType & IMAGE_REL_BASED_HIGHLOW)
			{
				uint32_t original;
				if (!getData(sc->data, si, original))
				{
					this->errorStream << "Failed to read original value to reloc!" << std::endl;
					return false;
				}
				putData(sc->data, si, original + relocDelta);
			}
			else if (entryType)
			{
				this->errorStream << "Unknown reloc type: 0x" << entryType << std::endl;
				return false;
			}

			numberOfRelocsPerformed++;
		}
	}

	this->infoStream << "\tParsed original reloc table and applied " << std::dec << numberOfRelocsPerformed << std::hex << " relocations" << std::endl;
	this->infoStream << "\t\tDelta of 0x" << relocDelta << " applied, as binary will load at 0x" << ACTUALIZED_BASE_ADDRESS << std::endl;

	/* we also need to clear out the original reloc table */
	while (reloc.calcNumberOfRelocations())
		reloc.removeRelocation(0);

	this->infoStream << "\tCleared original reloc table" << std::endl;

	return true;
}

bool PeRecompiler::rewriteHeader()
{
	if (!this->doRewriteReadyCheck())
		return false;

	if (!this->shouldUseWin10Attack)
	{
		this->addRewriteBlock<EntryPointRewriteBlock>(this->peFile);
		this->infoStream << "Rewrote header entrypoint" << std::endl;
	}
	else
	{
		this->infoStream << "[Win10 Attack] Skipping header entrypoint rewrite" << std::endl;
	}
	return true;
}

bool PeRecompiler::fixupBase()
{
	if (!this->doRewriteReadyCheck())
		return false;

	this->addRewriteBlock<BaseAddressRewriteBlock>(this->peFile);
	this->infoStream << "Added fixup rewrite for ImageBase; will match actual base in memory" << std::endl;

	return true;
}

bool PeRecompiler::rewriteSection(const std::string &name)
{
	if (!this->doRewriteReadyCheck())
		return false;

	for (auto isec = this->sectionContents.begin(); isec != this->sectionContents.end(); isec++)
	{
		auto& sec = *isec;
		if (sec->name.compare(name) == 0)
		{
			this->addRewriteBlock<PeSectionRewriteBlock>(sec);
			this->infoStream << "\tRewrote " << name << " section at RVA: 0x" << sec->RVA << std::endl;
			return true;
		}
	}

	this->infoStream << "\tSeemingly no section named " << name << " to rewrite" << std::endl;
	return true;
}

bool PeRecompiler::rewriteImports()
{
	if (!this->doRewriteReadyCheck())
		return false;

	if (this->shouldUseWin10Attack)
	{
		this->infoStream << "[Win10 Attack] Skipping import obfuscation" << std::endl;
		return true;
	}


	auto& peHeader = this->peFile->peHeader();
	this->infoStream << "Obfuscating imports" << std::endl;

	/* rewrite Import Address Table */
	uint32_t iatRVA = peHeader.getIddIatRva();
	uint32_t iatSize = peHeader.getIddIatSize();
	if (!this->rewriteSubsectionByRVA(iatRVA, iatSize))
		this->infoStream << "\tSeemingly no Import Address Table to rewrite" << std::endl;
	else
		this->infoStream << "\tRewrote Import Address Table from RVA 0x" << iatRVA << " to 0x" << (iatRVA + iatSize) << std::endl;

	/* rewrite Import Directory Table */
	uint32_t importRVA = peHeader.getIddImportRva();
	uint32_t importSize = peHeader.getIddImportSize();
	if (!this->rewriteSubsectionByRVA(importRVA, importSize))
		this->infoStream << "\tSeemingly no Import Table to rewrite" << std::endl;
	else
		this->infoStream << "\tRewrote Import Table from RVA 0x" << importRVA << " to 0x" << (importRVA + importSize) << std::endl;

	/* rewrite Import Hints/Names & Dll Names Table */
	auto iatSec = getSectionByRVA(iatRVA, iatSize);
	if (iatSec)
	{
		uint32_t iatOffset = iatRVA - iatSec->RVA;

		uint32_t lowestNameRVA = 0xFFFFFFFF;
		uint32_t highestNameRVA = 0;
		for (uint32_t imp = iatOffset; imp < iatOffset + iatSize; imp += 4)
		{
			uint32_t temp;
			if (!getData(iatSec->data, imp, temp))
				break;
			if (temp == 0) continue;
			else if (temp < lowestNameRVA) lowestNameRVA = temp;
			else if (temp > highestNameRVA) highestNameRVA = temp;
		}

		if (!this->rewriteSubsectionByRVA(lowestNameRVA, highestNameRVA - lowestNameRVA))
			this->infoStream << "\tSeemingly no Import Hints/Names & Dll Names Table to rewrite" << std::endl;
		else
			this->infoStream << "\tRewrote Import Hints/Names & Dll Names Table from RVA 0x" << lowestNameRVA << " to 0x" << highestNameRVA << std::endl;
	}

	return true;
}

bool PeRecompiler::rewriteMatches(const std::string &needle)
{
	if (!this->doRewriteReadyCheck())
		return false;

	this->infoStream << "\tObfuscating all instances of string: " << needle << std::endl;
	std::boyer_moore_horspool_searcher<std::string::const_iterator> searcher(needle.cbegin(), needle.cend());
	for (auto isec = this->sectionContents.begin(); isec != this->sectionContents.end(); isec++)
	{
		auto& sec = *isec;
		auto schunk = std::string(sec->data.begin(), sec->data.end());
		auto res = schunk.cbegin();
		while (true)
		{
			res = std::search(res, schunk.cend(), searcher);
			if (res == schunk.cend())
				break;
			auto index = (res - schunk.cbegin()) * sizeof(std::string::value_type);

			this->infoStream << "\t\tMatch in " << sec->name << " at offset 0x" << std::hex << index << std::endl;
			this->addRewriteBlock<PeSectionRewriteBlock>(sec, index, needle.length() + 1);
			res++;
		}
	}

	return true;
}


bool PeRecompiler::writeOutputFile()
{
	if (!this->peFile)
		return false;

	if (!this->sectionContents.size())
	{
		this->errorStream << "Section contents must be loaded before writing output!" << std::endl;
		return false;
	}

	this->infoStream << "Generating output file" << std::endl;

	auto& reloc = this->peFile->relocDir();
	auto& peHeader = this->peFile->peHeader();
	auto& mzHeader = this->peFile->mzHeader();

	/*
		first, we need to actually DO THE REWRITES that we have queued up.
		we will modify the contents buffers we have in rewriteBlocks and
		keep a ledger of those so we can generate a reloc table later.

		in order to support overlapping relocs, we must record blocks in the
		reverse order that we decrement them, as relocations are processed
		in a linear fashion. this is done using .push_front().

		this will work as long as each entry in rewriteBlocks contains _no overlap_,
		as the reversal happens between entries in rewriteBlocks, not between relocations
		in each entry.
	*/
	struct PackedBlock
	{
		PackedBlock(unsigned int _beginRVA) : beginRVA(_beginRVA), offsets() {}
		unsigned int beginRVA;
		std::vector<unsigned short> offsets;
	};
	std::list<PackedBlock> packedBlocks;

	const uint32_t requestedBase = peHeader.getImageBase();
	const uint32_t packDelta = (ACTUALIZED_BASE_ADDRESS - requestedBase);
	const uint32_t dataSize = 4;
	const uint32_t chunkSize = 1024 * dataSize;
	for (auto iblock = this->rewriteBlocks.begin(); iblock != this->rewriteBlocks.end(); iblock++)
	{
		auto& block = *iblock;
		if (!block)
			continue;

		uint32_t rva, offset;
		if (!block->getFirstEntryLoc(dataSize, rva, offset))
			continue;
		packedBlocks.push_front(PackedBlock(rva));

		do
		{
			if (!block->decrementEntry(offset, packDelta))
				break;

			auto packedBlock = packedBlocks.begin();
			auto rvaOffset = static_cast<uint16_t>((rva - packedBlock->beginRVA));
			if (rvaOffset >= chunkSize)
			{
				rvaOffset = 0;
				packedBlocks.push_front(PackedBlock(rva));
				packedBlock = packedBlocks.begin();
			}

			packedBlock->offsets.push_back(rvaOffset);
		}
		while (block->getNextEntryLoc(dataSize, offset, rva, offset));
	}

	/* now that that's done, we actually need to generate a reloc table... */
	if (packedBlocks.size())
	{
		this->infoStream << "\tApplied all rewrites to actual file contents" << std::endl;

		if (reloc.calcNumberOfRelocations())
		{
			this->errorStream << "No relocation table should exist if rewrites are present!" << std::endl;
			return false;
		}

		/* generate a new reloc entry for each packed block */
		for (auto ipb = packedBlocks.begin(); ipb != packedBlocks.end(); ipb++)
		{
			auto& packedBlock = *ipb;

			unsigned int rel = reloc.calcNumberOfRelocations();
			reloc.addRelocation();

			for (auto offset = packedBlock.offsets.begin(); offset != packedBlock.offsets.end(); offset++)
				reloc.addRelocationData(rel, (IMAGE_REL_BASED_HIGHLOW << 12) | (*offset & 0x0FFF));

			/*
				calculate the header values and write them.
				size needs +8 to include headr itself.
				number of entries should be even to align entire table on 4-byte boundary.
			*/
			uint32_t relocSize = (packedBlock.offsets.size() * sizeof(uint16_t)) + 8;
			if (packedBlock.offsets.size() % 2 == 1)
			{
				reloc.addRelocationData(rel, 0);
				relocSize += sizeof(uint16_t);
			}
			reloc.setVirtualAddress(rel, packedBlock.beginRVA);
			reloc.setSizeOfBlock(rel, relocSize);
		}

		this->infoStream << "\tGenerated reloc table for rewrites with " << std::dec << packedBlocks.size() << std::hex << " entries" << std::endl;
	}


	/*
		embed the new reloc table in place of the old one.
		make sure the raw data is aligned on a 512-byte boundary for filesystem mapping.
	*/
	auto relocSec = this->getSectionByRVA(peHeader.getIddBaseRelocRva(), 4);
	if (!relocSec)
	{
		this->errorStream << "Failed to locate reloc section!" << std::endl;
		return false;
	}

	relocSec->data.clear();
	reloc.rebuild(relocSec->data);
	peHeader.setVirtualSize(relocSec->index, relocSec->data.size());
	peHeader.setIddBaseRelocSize(relocSec->data.size());
	while (relocSec->data.size() % 512)
		relocSec->data.push_back(0x00);
	peHeader.setSizeOfRawData(relocSec->index, relocSec->data.size());

	this->infoStream << "\tUpdated PE header with new reloc meta-data" << std::endl;

	/* validate the binary since we changed some sections */
	peHeader.makeValid(mzHeader.getAddressOfPeHeader());
	this->infoStream << "\tValidated new PE header" << std::endl;

	/* inject preselection shellcode, if needed */
	if (this->shouldUseWin10Attack)
	{
		this->infoStream << "\t[Win10 Attack] Injecting ASLR preselection stub" << std::endl;

		/* prepare the stub */
		void* originalEntrypoint = (void*)peHeader.getAddressOfEntryPoint();

		void* stub;
		size_t stubLen;
		if (!prepareStub(originalEntrypoint, stub, stubLen, this->infoStream, this->errorStream))
		{
			this->errorStream << "Failed to inject ASLR preselection stub!" << std::endl;
			return false;
		}

		/* inject a section to hold the stub */
		auto access = PeLib::PELIB_IMAGE_SCN_MEM_EXECUTE | PeLib::PELIB_IMAGE_SCN_MEM_WRITE |
						PeLib::PELIB_IMAGE_SCN_MEM_READ | PeLib::PELIB_IMAGE_SCN_CNT_INITIALIZED_DATA |
						PeLib::PELIB_IMAGE_SCN_CNT_CODE;
		auto sc = this->allocSection(".presel", stubLen, access);

		peHeader.addSection(".presel", stubLen);

		/* point the entrypoint at the stub */
		auto offsetOfNewSection = peHeader.rvaToOffset(sc->RVA);
		this->infoStream << "\t\tOriginal EP: 0x" << std::hex << originalEntrypoint << std::endl;
		this->infoStream << "\t\tStub Section RVA: 0x" << std::hex << sc->RVA << std::endl;
		this->infoStream << "\t\tStub Section Offset: 0x" << std::hex << offsetOfNewSection << std::endl;
		peHeader.setAddressOfEntryPoint(sc->RVA);
		this->infoStream << "\t\tEP updated to RVA" << std::endl;

		/* write the stub to the section */
		pushBytes((const char*)stub, stubLen, sc->data);
	}
	
	/* write original MZ and PE headers to new binary */
	mzHeader.write(this->outputFileName, 0);
	this->infoStream << "\tWrote MZ Header to output file" << std::endl;

    peHeader.write(this->outputFileName, mzHeader.getAddressOfPeHeader());
	this->infoStream << "\tWrote PE Header to output file" << std::endl;
        
	/* write section meta-data and contents to new binary */
    peHeader.writeSections(this->outputFileName);
	this->infoStream << "\tWrote PE Section meta-data to output file" << std::endl;

	for (auto isec = this->sectionContents.begin(); isec != this->sectionContents.end(); isec++)
	{
		auto& sec = *isec;
		if (sec->size)
			peHeader.writeSectionData(this->outputFileName, sec->index, sec->data);
	}
	this->infoStream << "\tWrote PE Section Contents to output file" << std::endl;

	return true;
}


bool PeRecompiler::doRewriteReadyCheck()
{
	if (!this->peFile)
		return false;

	if (!this->sectionContents.size())
	{
		this->errorStream << "Section contents must be loaded before doing rewrites!" << std::endl;
		return false;
	}

	if (this->peFile->relocDir().calcNumberOfRelocations() || this->peFile->peHeader().getImageBase() != TRICKY_BASE_ADDRESS)
	{
		if (!this->shouldUseWin10Attack)
		{
			this->errorStream << "On-disk relocations must be performed before doing rewrites!" << std::endl;
			return false;
		}
	}

	return true;
}

std::shared_ptr<PeSectionContents> PeRecompiler::getSectionByRVA(uint32_t RVA, uint32_t size)
{
	if (!RVA || !size)
		return nullptr;

	for (auto isec = this->sectionContents.begin(); isec != this->sectionContents.end(); isec++)
	{
		auto& sec = *isec;
		if (RVA < sec->RVA || RVA >= (sec->RVA + sec->size) || (RVA + size) > (sec->RVA + sec->size))
			continue;
		return sec;
	}
	return nullptr;
}

std::shared_ptr<PeSectionContents> PeRecompiler::allocSection(const std::string& name, uint32_t size, uint32_t access)
{
	auto& peHeader = this->peFile->peHeader();
	auto& mzHeader = this->peFile->mzHeader();

	/*
		if possible, find an unused section to reuse.

		this works in theory, but needs to be properly tested
		and, as such, is disabled. my major concern right now
		is that we may re-use a section that is marked as discardable,
		which is typically a property of .reloc, which is also the only
		section which would currently be a reuse candidate. (fancy pooling
		just being an abstraction for future, even though there's only one)

		I am concerned about this because discardable sections may not have space
		alloted for them in the in-memory mapping, so reusing them for things
		which must not be discarded, such as injected stubs, can be
		problematic. This isn't always the case. For instance, if the section is
		the final section in the binary, there shouldn't be an issue. Might also
		be able to mess with VA's and such for other cases.

		In short, this is here because I want it to work, but it currently
		needs more testing and consideration. You might say "wait, the code
		is right here, what do you mean this is disabled?" And the answer is
		that we never add anything to this->sectionPool, so the loop won't run.
	*/
	std::shared_ptr<PeSectionContents> newSec = nullptr;
	auto finalSecIndex = peHeader.getNumberOfSections() - 1;
	for (auto& sec : this->sectionPool)
	{
		if (sec->size > size || sec->index == finalSecIndex)
		{
			this->infoStream << "\t\tRepurposed Section " << sec->name << " as " << name << std::endl;
			newSec = sec;
			peHeader.setSectionName(sec->index, name);
			peHeader.setVirtualSize(sec->index, size);
			peHeader.setSizeOfRawData(sec->index, size);
			this->sectionPool.remove(sec);
			break;
		}
	}

	/* if reuse failed, alloc a new section */
	if (!newSec)
	{
		/* allocate it */
		peHeader.addSection(name, size);
		this->infoStream << "\t\tInjected Section " << name << std::endl;

		/* re-validate the binary since we added a section */
		peHeader.makeValid(mzHeader.getAddressOfPeHeader());

		/* track the section */
		newSec = std::make_shared<PeSectionContents>();
		newSec->index = peHeader.getNumberOfSections() - 1;
		this->sectionContents.push_back(newSec);
	}

	/* set up proper section access */
	peHeader.setCharacteristics(newSec->index, access);

	/* prepare the section contents */
	newSec->RVA = peHeader.getVirtualAddress(newSec->index);
	newSec->size = peHeader.getSizeOfRawData(newSec->index);
	newSec->rawPointer = peHeader.getPointerToRawData(newSec->index);
	newSec->virtualSize = peHeader.getVirtualSize(newSec->index);
	newSec->name = peHeader.getSectionName(newSec->index);

	/* display some info and nope out */
	this->infoStream << "\t\t\tVirtual Size: 0x" << newSec->virtualSize << std::endl;
	this->infoStream << "\t\t\tRVA: 0x" << newSec->RVA << std::endl;
	this->infoStream << "\t\t\tRaw Size: 0x" << newSec->size << std::endl;
	this->infoStream << "\t\t\tRaw Pointer: 0x" << newSec->rawPointer << std::endl;

	return newSec;
}

bool PeRecompiler::rewriteSubsectionByRVA(uint32_t RVA, uint32_t size)
{
	auto sec = getSectionByRVA(RVA, size);
	if (!sec)
		return false;
	this->addRewriteBlock<PeSectionRewriteBlock>(sec, RVA - sec->RVA, size);
	return true;
}