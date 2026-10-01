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

const char* CEngine::GetVersionMinor() const
{
	return sAICallback->Engine_Version_getMinor(skirmishAIId);
}

const char* CEngine::GetVersionPatchset() const
{
	return sAICallback->Engine_Version_getPatchset(skirmishAIId);
}

const char* CEngine::GetVersionCommits() const
{
	return sAICallback->Engine_Version_getCommits(skirmishAIId);
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

const char* CEngine::GetVersionNormal() const
{
	return sAICallback->Engine_Version_getNormal(skirmishAIId);
}

const char* CEngine::GetVersionSync() const
{
	return sAICallback->Engine_Version_getSync(skirmishAIId);
}

const char* CEngine::GetVersionFull() const
{
	return sAICallback->Engine_Version_getFull(skirmishAIId);
}

bool CEngine::IsVersionRelease() const
{
	return sAICallback->Engine_Version_isRelease(skirmishAIId);
}

} // namespace circuit
