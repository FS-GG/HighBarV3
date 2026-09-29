/*
 * SpringEngine.cpp
 *
 *  Created on: Nov 8, 2019
 *      Author: rlcevg
 */

#include "spring/SpringEngine.h"

#include "SSkirmishAICallback.h"	// "direct" C API

namespace circuit {

CEngine::CEngine(const struct SSkirmishAICallback* clb, int sAIId)
		: sAICallback(clb)
		, skirmishAIId(sAIId)
{
}

CEngine::~CEngine()
{
}

const char* CEngine::GetVersionMajor() const
{
	return sAICallback->Engine_Version_getMajor(skirmishAIId);
}

const char* CEngine::GetVersionHash() const
{
	return sAICallback->Engine_Version_getHash(skirmishAIId);
}

const char* CEngine::GetVersionBranch() const
{
	return sAICallback->Engine_Version_getBranch(skirmishAIId);
}

const char* CEngine::GetVersionAdditional() const
{
	return sAICallback->Engine_Version_getAdditional(skirmishAIId);
}

} // namespace circuit
