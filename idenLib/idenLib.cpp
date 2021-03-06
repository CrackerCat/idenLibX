#include "idenLib.h"
#include "compression.h"

// opcodes, <name, numberOfBranches>
std::unordered_map<std::string, std::tuple<std::string, int>> funcSignature;
// opcodes, <name, instrsFromFunc, instrsFromEP>
std::unordered_map<std::string, std::tuple<std::string, size_t, signed long>> mainSig;

int nBranches{};

/* 
 * example: 11abff03 => \x11\xab\xff\x03 
 * 
 */
uint8_t* ConvertToRawHexString(__in const std::string& inputStr)
{
	const auto outPtr = new uint8_t[inputStr.size() / 2 + 1]{};
	if (!outPtr)
		return nullptr;
	auto tmpBuff = outPtr;
	for (size_t i = 0; i < inputStr.size(); i += 2)
	{
		*(tmpBuff++) = std::strtoul(inputStr.substr(i, 2).c_str(), nullptr, 16);
	};
	tmpBuff = nullptr;

	return outPtr;
}

std::vector<uint8_t> ConvertToRawHexVector(__in const std::string& inputStr)
{
	std::vector<uint8_t> vec;

	for (size_t i = 0; i < inputStr.size(); i += 2)
	{
		const auto tmpChr = std::strtoul(inputStr.substr(i, 2).c_str(), nullptr, 16);
		vec.push_back(tmpChr);
	};

	return vec;
}

_Success_(return)
bool GetOpcodeBuf(__in PBYTE funcVa, __in SIZE_T length, __out PCHAR& opcodesBuf, __in bool countBranches, __out int& cBranches)
{
	ZydisDecoder decoder;

	ZydisDecoderInit(&decoder, ZYDIS_MODE, ZYDIS_ADDRESS_WIDTH);

	ZyanUSize offset = 0;
	ZydisDecodedInstruction instruction;

	cBranches = 0;

	auto cSize = length * 2;
	opcodesBuf = static_cast<PCHAR>(malloc(cSize)); // // we need to resize the buffer
	if (!opcodesBuf)
	{
		return false;
	}
	SIZE_T counter = 0;
	while (		ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, funcVa + offset, length - offset,
		&instruction)))
	{
		CHAR opcode[3];
		sprintf_s(opcode, "%02x", instruction.opcode);

		memcpy_s(opcodesBuf + counter, cSize - counter, opcode, sizeof(opcode));
		counter += 2;

		if (countBranches)
		{
			if (instruction.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
				cBranches++;
			}
		}

		offset += instruction.length;
	}
	auto tmpPtr = static_cast<PCHAR>(realloc(opcodesBuf, counter + 1)); // +1 for 0x00
	if (!tmpPtr)
		return false;
	opcodesBuf = tmpPtr;

	return counter != 0;
}

// http://www.martinbroadhurst.com/how-to-split-a-string-in-c.html
void Split(__in const std::string& str, __out std::vector<std::string>& cont)
{
	std::istringstream iss(str);
	std::copy(std::istream_iterator<std::string>(iss),
	          std::istream_iterator<std::string>(),
	          std::back_inserter(cont));
}

bool getSig(__in const fs::path& sigPath)
{
	PBYTE decompressedData = nullptr;
	if (!DecompressFile(sigPath, decompressedData) || !decompressedData)
	{
		return false;
	}
	char seps[] = "\n";
	char* next_token = nullptr;
	char* line = strtok_s(reinterpret_cast<char*>(decompressedData), seps, &next_token);
	while (line != nullptr)
	{
		// vec[0] opcode
		// vec[1] name
		std::vector<std::string> vec{};
		Split(line, vec);
		if (vec.size() != 2)
		{
			return false;
		}

		// check "main"
		auto isMain = vec[0].find('_');
		auto isBranch = vec[0].find('+');
		if (std::string::npos != isMain) // it's main
		{
			auto indexStr = std::string(vec[0].begin() + isMain + 1, vec[0].end());
			auto opcodeString = std::string(vec[0].begin(), vec[0].begin() + isMain);
			auto isEP = indexStr.find('!');
			if (std::string::npos != isEP)
			{
				auto fromEPStr = std::string(indexStr.begin() + isEP + 1, indexStr.end());
				auto fromFuncStr = std::string(indexStr.begin(), indexStr.begin() + isEP);
				signed long fromEP = std::stoi(fromEPStr);
				size_t fromFunc = std::stoi(fromFuncStr);

				mainSig[opcodeString] = std::make_tuple(vec[1], fromFunc, fromEP);
			}
		}
		else if (std::string::npos != isBranch)
		{
			auto opcodes = std::string(vec[0].begin(), vec[0].begin() + isBranch);
			auto strBranches = std::string(vec[0].begin() + isBranch + 1, vec[0].end());
			auto nBranches = std::stoi(strBranches);
			funcSignature[opcodes] = std::make_tuple(vec[1], nBranches);
		}
		else
		{
			funcSignature[vec[0]] = std::make_tuple(vec[1], 0);
		}
		line = strtok_s(nullptr, seps, &next_token);
	}


	delete[] decompressedData;

	return true;
}

void Analyze()
{
	DbgCmdExecDirect("analyze"); // Do function analysis.
	DbgCmdExecDirect("analyse_nukem"); // Do function analysis using nukem�s algorithm.
}

void CacheSigs(const fs::path& cachePath, const fs::path& cachePathMain)
{
	auto funcFuture = std::async([](const fs::path& cachePath, const fs::path& cachePathMain)
	{
		std::ofstream is(cachePath, std::ios::binary);
		cereal::BinaryOutputArchive archive(is);
		archive(funcSignature);
	}, cachePath, cachePathMain);

	auto mainsFuture = std::async([](const fs::path& cachePath, const fs::path& cachePathMain)
	{
		std::ofstream isMain(cachePathMain, std::ios::binary);
		cereal::BinaryOutputArchive archiveMain(isMain);
		archiveMain(mainSig);
	}, cachePath, cachePathMain);

	// no need to wait...
}

void ParseSignatures(const fs::path& cachePath, const fs::path& cachePathMain)
{
	// else parse signature files
	const fs::path sigDir{ SymExDir };
	std::error_code ec{};
	for (auto& p : fs::recursive_directory_iterator(sigDir, ec))
	{
		if (ec.value() != STATUS_SUCCESS)
		{
			continue;
		}
		const auto& currentPath = p.path();
		if (is_regular_file(currentPath, ec))
		{
			if (ec.value() != STATUS_SUCCESS)
			{
				continue;
			}

			if (currentPath.extension().compare(SIG_EXT) == 0)
			{
				getSig(currentPath);
			}
		}
	}
	// cache it
	CacheSigs(cachePath, cachePathMain);
}

void getSignatures()
{
	fs::path cachePath = { SymExDir };
	cachePath += "\\";
	auto cachePathMain = cachePath;
	cachePath += idenLibCache;
	cachePathMain += idenLibCacheMain;

	if (exists(cachePath))
	{
		{
			std::ifstream is(cachePath, std::ios::binary);
			cereal::BinaryInputArchive inputArchive(is);
			inputArchive(funcSignature);
		}
		if (exists(cachePathMain))
		{
			std::ifstream is(cachePathMain, std::ios::binary);
			cereal::BinaryInputArchive inputArchive(is);
			inputArchive(mainSig);
		}
	}
	else
	{
		ParseSignatures(cachePath, cachePathMain);
	}
}

void ProcessSignatures()
{
	size_t counter = 0;
	ListInfo functionList{};
	if (!Script::Function::GetList(&functionList))
	{
		_plugin_logprintf("[idenLib - FAILED] Failed to get list of functions\n");
		return;
	}
	const auto fList = static_cast<Script::Function::FunctionInfo *>(functionList.data);

	const auto moduleBase = Script::Module::GetMainModuleBase();
	const auto moduleSize = DbgFunctions()->ModSizeFromAddr(moduleBase);
	const auto moduleMemory = static_cast<PBYTE>(Script::Misc::Alloc(moduleSize));

	if (!DbgMemRead(moduleBase, moduleMemory, moduleSize))
	{
		_plugin_logprintf("[idenLib - FAILED] Couldn't read process memory for scan\n");
		return;
	}

	// get signatures
	// check if cache exists
	getSignatures();

	// apply sig
	auto mainDetected = false;
	for (auto i = 0; i < functionList.count; i++)
	{
		const auto codeStart = moduleBase + fList[i].rvaStart;

		auto codeSize = fList[i].rvaEnd - fList[i].rvaStart + 1;
		if (codeSize < MIN_FUNC_SIZE)
			continue;
		if (codeSize > MAX_FUNC_SIZE)
		{
			codeSize = MAX_FUNC_SIZE;
		}

		PCHAR opcodesBuf = nullptr;
		auto codeStartMod = moduleMemory + fList[i].rvaStart;
		if (GetOpcodeBuf(codeStartMod, codeSize, opcodesBuf, false, nBranches) && opcodesBuf)
		{
			// library functions
			std::string cOpcodes{opcodesBuf};
			if (funcSignature.find(cOpcodes) != funcSignature.end())
			{
				DbgSetAutoLabelAt(codeStart, std::get<0>(funcSignature[cOpcodes]).c_str());
				counter++;
			}

			// "main" function
			if (!mainDetected && mainSig.find(cOpcodes) != mainSig.end()) // "main" func caller
			{
				auto callInstr = codeStartMod + std::get<1>(mainSig[cOpcodes]);

				ZydisDecodedInstruction instruction;
				ZydisDecoder decoder;

				ZydisDecoderInit(&decoder, ZYDIS_MODE, ZYDIS_ADDRESS_WIDTH);
				if (					ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, callInstr, codeSize,
					&instruction)))
				{
					if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
					{
						auto& callOperand = instruction.operands[0];
						ZyanU64 callVa{};
						auto instr = reinterpret_cast<ZyanU64>(callInstr);
						if (callOperand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && callOperand.imm.is_relative &&
							ZYAN_SUCCESS(
							ZydisCalcAbsoluteAddress(&instruction, &callOperand, instr, &callVa)))
						{
							auto realCallVa = callVa - reinterpret_cast<DWORD_PTR>(codeStartMod) + codeStart;
							DbgSetAutoLabelAt(realCallVa, std::get<0>(mainSig[cOpcodes]).c_str());
							counter++;
							mainDetected = true;
						}
					}
				}
			}

			free(opcodesBuf);
		}
	}

	// Alternative way to recognize a main function
	if (!mainDetected)
	{
		DWORD_PTR EPAddress = Script::Module::GetMainModuleEntry();
		DWORD_PTR EPAddressMod = EPAddress - moduleBase + reinterpret_cast<DWORD_PTR>(moduleMemory);
		for (const auto& sig : mainSig)
		{
			DWORD_PTR callInstr = std::get<2>(sig.second) + EPAddressMod;
			auto mainOp = sig.first.c_str();
			ZydisDecodedInstruction instruction;
			ZydisDecoder decoder;

			ZydisDecoderInit(&decoder, ZYDIS_MODE, ZYDIS_ADDRESS_WIDTH);
			if (				ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, reinterpret_cast<PVOID>(callInstr), MAX_FUNC_SIZE,
				&instruction)))
			{
				if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
				{
					auto fromFunc = std::get<1>(sig.second);
					auto funcStart = callInstr - fromFunc;
					PCHAR opcodesBuf = nullptr;
					if (GetOpcodeBuf(reinterpret_cast<PBYTE>(funcStart), MAX_FUNC_SIZE, opcodesBuf, false, nBranches) && opcodesBuf)
					{
						if (!strncmp(opcodesBuf, mainOp, strlen(mainOp)))
						{
							auto& callOperand = instruction.operands[0];
							ZyanU64 callVa{};
							auto instr = static_cast<ZyanU64>(callInstr);
							if (callOperand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && callOperand.imm.is_relative &&
								ZYAN_SUCCESS(
								ZydisCalcAbsoluteAddress(&instruction, &callOperand, instr, &callVa)))
							{
								auto realCallVa = callVa - reinterpret_cast<DWORD_PTR>(moduleMemory) + moduleBase;
								DbgSetAutoLabelAt(realCallVa, std::get<0>(sig.second).c_str());
								counter++;
								mainDetected = true;
								break;
							}
						}
						free(opcodesBuf);
					}
				}
			}
		}
	}

	char msg[0x100]{};
	sprintf_s(msg, "\n[idenLib - Exact Match] Applied to %zd function(s)\n", counter);
	GuiAddLogMessage(msg);

	Script::Misc::Free(moduleMemory);
	BridgeFree(functionList.data);
	GuiUpdateDisassemblyView();
}

float JaccardSimilarity(const uint8_t* v1, const uint8_t* v2)
{
	uint8_t bitMap1[256]{};
	uint8_t bitMap2[256]{};
	for (auto i = 0; v1[i]; i++) {
		bitMap1[v1[i]] = 1;
	}
	for (auto i = 0; v2[i]; i++) {
		bitMap2[v2[i]] = 1;
	}
	auto in = 0;
	auto un = 0;


	for (auto i = 0; i < 256; i++) {
		in += bitMap1[i] && bitMap2[i];
		un += bitMap1[i] || bitMap2[i];
	}

	const auto jaccard = static_cast<float>(in) / un;

	return jaccard;
}

double CosineSimilarity(std::vector<uint8_t> inputFirst, std::vector<uint8_t> inputSecond)
{
	auto mul = 0, d_a = 0, d_b = 0;

	if (inputFirst.size() != inputSecond.size())
	{
		throw std::logic_error("Vector inputFirst and Vector inputSecond are not the same size");
	}

	// Prevent Division by zero
	if (inputFirst.empty())
	{
		throw std::logic_error("Vector inputFirst and Vector inputSecond are empty");
	}

	auto B_iter = inputSecond.begin();
	auto A_iter = inputFirst.begin();
	for (; A_iter != inputFirst.end(); ++A_iter, ++B_iter)
	{
		mul += *A_iter * *B_iter;
		d_a += *A_iter * *A_iter;
		d_b += *B_iter * *B_iter;
	}

	if (d_a == 0 || d_b == 0)
	{
		throw std::logic_error(
			"cosine similarity is not defined whenever one or both "
			"input vectors are zero-vectors.");
	}

	return mul / (sqrt(d_a) * sqrt(d_b));
}

void ProcessSignaturesJaccard()
{
	size_t counter = 0;
	ListInfo functionList{};
	if (!Script::Function::GetList(&functionList))
	{
		_plugin_logprintf("[idenLib - FAILED] Failed to get list of functions\n");
		return;
	}
	const auto fList = static_cast<Script::Function::FunctionInfo *>(functionList.data);

	const auto moduleBase = Script::Module::GetMainModuleBase();
	const auto moduleSize = DbgFunctions()->ModSizeFromAddr(moduleBase);
	const auto moduleMemory = static_cast<PBYTE>(Script::Misc::Alloc(moduleSize));

	if (!DbgMemRead(moduleBase, moduleMemory, moduleSize))
	{
		_plugin_logprintf("[idenLib - FAILED] Couldn't read process memory for scan\n");
		return;
	}

	// get signatures
	// check if cache exists
	getSignatures();

	// apply sig
	for (auto i = 0; i < functionList.count; i++)
	{
		const auto codeStart = moduleBase + fList[i].rvaStart;

		auto codeSize = fList[i].rvaEnd - fList[i].rvaStart + 1;
		if (codeSize < MIN_FUNC_SIZE)
			continue;
		if (codeSize > MAX_FUNC_SIZE)
		{
			codeSize = MAX_FUNC_SIZE;
		}

		PCHAR opcodesBuf = nullptr;
		const auto codeStartMod = moduleMemory + fList[i].rvaStart;
		if (GetOpcodeBuf(codeStartMod, codeSize, opcodesBuf, true, nBranches) && opcodesBuf)
		{
			// library functions
			std::string cOpcodes{ opcodesBuf };
			for (const auto& sig : funcSignature)
			{
				const auto& sigBranches = std::get<1>(sig.second);
				const auto diffBranch = std::abs(sigBranches - nBranches);
				if (diffBranch > 2)
				{
					free(opcodesBuf);
					continue;
				}

				const int sigSize = sig.first.size();
				const int opcSize = cOpcodes.size();
				const auto diffSize = std::abs(sigSize - opcSize);
				if (diffSize > 5)
				{
					free(opcodesBuf);
					continue;
				}

				const auto sigPtr = ConvertToRawHexString(sig.first);
				const auto opcPtr = ConvertToRawHexString(cOpcodes);

				//auto sigVec = ConvertToRawHexVector(sig.first);
				//auto opcVec = ConvertToRawHexVector(cOpcodes);

				if (sigPtr && opcPtr)
				{
				
					//if (sigVec.size() > opcVec.size())
					//	opcVec.resize(sigVec.size());
					//else
					//	sigVec.resize(opcVec.size());
					//const auto cosineResult = CosineSimilarity(sigVec, opcVec);

					const auto jaccardResult = JaccardSimilarity(sigPtr, opcPtr);
					if (jaccardResult >= JACCARD_DISTANCE)
					{
						const auto& labelName = std::get<0>(sig.second);

						///////// for testing
						//{
						//	char msg[0x200 + MAX_LABEL_SIZE]{};
						//	char label_text[MAX_LABEL_SIZE] = "";
						//	DbgGetLabelAt(codeStart, SEG_DEFAULT, label_text);
						//	sprintf_s(msg, "\nold: %s new: %s\n%s : %s\n", label_text, labelName.c_str(), sig.first.c_str(), cOpcodes.c_str());
						//	GuiAddLogMessage(msg);
						//}

						DbgSetAutoLabelAt(codeStart, labelName.c_str());
						counter++;
					}
				}
			}

			free(opcodesBuf);
		}
	}

	char msg[0x100]{};
	sprintf_s(msg, "\n[idenLib - Jaccard Similarity] Applied to %zd function(s)\n", counter);
	GuiAddLogMessage(msg);

	Script::Misc::Free(moduleMemory);
	BridgeFree(functionList.data);
	GuiUpdateDisassemblyView();
}

bool cbIdenLib(int argc, char* argv[])
{
	if (!DbgIsDebugging())
	{
		_plugin_logprintf("[idenLib] The debugger is not running!\n");
		return false;
	}
	const auto startTime = clock();

	Analyze();

	const fs::path sigFolder{SymExDir};
	if (!exists(sigFolder))
	{
		const auto path = absolute(sigFolder).string().c_str();
		GuiAddLogMessage("[idenLib - FAILED] Following path does not exist:");
		GuiAddLogMessage(path);
		return false;
	}

	ProcessSignatures();

	const auto endTime = clock();

	char msg[0x100]{};
	sprintf_s(msg, "[idenLib] Time Wasted %f Seconds\n", (static_cast<double>(endTime) - startTime) / CLOCKS_PER_SEC);
	GuiAddLogMessage(msg);

	return true;
}

bool IdenLibJaccard(int argc, char* argv[])
{
	if (!DbgIsDebugging())
	{
		_plugin_logprintf("[idenLib] The debugger is not running!\n");
		return false;
	}
	const auto startTime = clock();

	Analyze();

	const fs::path sigFolder{ SymExDir };
	if (!exists(sigFolder))
	{
		const auto path = absolute(sigFolder).string().c_str();
		GuiAddLogMessage("[idenLib - FAILED] Following path does not exist:");
		GuiAddLogMessage(path);
		return false;
	}

	ProcessSignaturesJaccard();

	const auto endTime = clock();

	char msg[0x100]{};
	sprintf_s(msg, "[idenLib] Time Wasted %f Seconds\n", (static_cast<double>(endTime) - startTime) / CLOCKS_PER_SEC);
	GuiAddLogMessage(msg);

	return true;
}

bool cbRefresh(int argc, char* argv[])
{
	// get signatures
	fs::path cachePath = { SymExDir };
	cachePath += "\\";
	auto cachePathMain = cachePath;
	cachePath += idenLibCache;
	cachePathMain += idenLibCacheMain;
	ParseSignatures(cachePath, cachePathMain);

	return true;
}
