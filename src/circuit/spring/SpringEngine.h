/*
 * SpringEngine.h
 *
 *  Created on: Nov 8, 2019
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_SPRING_SPRINGENGINE_H_
#define SRC_CIRCUIT_SPRING_SPRINGENGINE_H_

struct SSkirmishAICallback;

namespace circuit {

class CEngine
{
public:
	CEngine(const struct SSkirmishAICallback* clb, int sAIId);
	virtual ~CEngine();

	const char* GetVersionMajor() const;
	const char* GetVersionMinor() const;
	const char* GetVersionPatchset() const;
	const char* GetVersionCommits() const;
	const char* GetVersionHash() const;
	const char* GetVersionBranch() const;
	const char* GetVersionAdditional() const;
	const char* GetVersionNormal() const;
	const char* GetVersionSync() const;
	const char* GetVersionFull() const;
	bool IsVersionRelease() const;

private:
	const struct SSkirmishAICallback* sAICallback;
	int skirmishAIId;
};

} // namespace circuit

#endif // SRC_CIRCUIT_SPRING_SPRINGENGINE_H_
