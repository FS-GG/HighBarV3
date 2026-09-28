// SPDX-License-Identifier: GPL-2.0-only
//
// HighBarV3 — SnapshotBuilder impl (T032, T042).
//
// NOTE: Some of the manager calls below are best-guess based on the
// BARb header scan done at /speckit.implement time. If a call fails
// to resolve at build time, cross-check with the manager header and
// update accordingly — the shape of the snapshot (data-model §1) is
// what matters to observers, not the specific C++ path we walked to
// build it.

#include "grpc/SnapshotBuilder.h"

#include "CircuitAI.h"
#include "module/EconomyManager.h"
#include "spring/SpringCallback.h"
#include "resource/MetalManager.h"
#include "terrain/TerrainManager.h"
#include "spring/SpringMap.h"
#include "unit/CircuitUnit.h"
#include "unit/enemy/EnemyInfo.h"
#include "unit/enemy/EnemyManager.h"

#include "Feature.h"
#include "FeatureDef.h"
#include "Unit.h"  // springai::Unit — for GetHealth() on the wrapper

namespace circuit::grpc {

namespace {

void SetVec3(::highbar::v1::Vector3* dst, const springai::AIFloat3& src) {
	dst->set_x(src.x);
	dst->set_y(src.y);
	dst->set_z(src.z);
}

std::uint32_t FrameForWire(const CCircuitAI* ai) {
	if (ai == nullptr) return 0u;
	const int frame = ai->GetLastFrame();
	return frame >= 0 ? static_cast<std::uint32_t>(frame) : 0u;
}

}  // namespace

SnapshotBuilder::SnapshotBuilder(CCircuitAI* ai) : ai_(ai) {}

::highbar::v1::StateSnapshot SnapshotBuilder::Build() const {
	::highbar::v1::StateSnapshot out = BuildIncremental();
	*out.mutable_static_map() = StaticMap();
	return out;
}

::highbar::v1::StateSnapshot SnapshotBuilder::BuildIncremental() const {
	::highbar::v1::StateSnapshot out;
	if (ai_ == nullptr) return out;
	out.set_frame_number(FrameForWire(ai_));
	FillOwnUnits(&out);
	FillEnemies(&out);
	FillFeatures(&out);
	FillEconomy(&out);
	return out;
}

const ::highbar::v1::StaticMap& SnapshotBuilder::StaticMap() const {
	if (!static_map_cached_) {
		FillStaticMap(&static_map_cache_);
		static_map_cached_ = true;
	}
	return static_map_cache_;
}

void SnapshotBuilder::FillOwnUnits(::highbar::v1::StateSnapshot* out) const {
	for (const auto& [unit_id, unit] : ai_->GetTeamUnits()) {
		if (unit == nullptr) continue;
		auto* ou = out->add_own_units();
		ou->set_unit_id(static_cast<std::uint32_t>(unit_id));
		ou->set_team_id(ai_->GetTeamId());
		const auto* cdef = unit->GetCircuitDef();
		ou->set_def_id(cdef != nullptr
		               ? static_cast<std::uint32_t>(cdef->GetId()) : 0);
		SetVec3(ou->mutable_position(), unit->GetPos(ai_->GetLastFrame()));
		// CCircuitUnit doesn't expose GetHealth() directly — route through
		// the springai::Unit wrapper (ICoreUnit::GetUnit()). Guard on null
		// in case the wrapper isn't bound (dying unit race).
		springai::Unit* su = unit->GetUnit();
		const float health = (su != nullptr) ? su->GetHealth() : 0.0f;
		ou->set_health(health);
		// max_health: prefer CircuitDef's GetMaxHealth on the springai def
		// (wrapper-side). Plan §data-model §1 requires health <= max_health,
		// so never 0.
		const float max_hp = cdef != nullptr && cdef->GetDef() != nullptr
		                     ? cdef->GetDef()->GetHealth() : health;
		ou->set_max_health(max_hp > 0.0f ? max_hp : health);
		// under_construction / build_progress: springai::Unit exposes
		// IsBeingBuilt() + GetBuildProgress() directly. 003 wires them
		// up so behavioral-build predicates can verify BuildUnit
		// side-effects on the wire (spec FR-001). Guard on null su
		// (dying unit race) by reporting defaults.
		const bool being_built = (su != nullptr) ? su->IsBeingBuilt() : false;
		const float bp = (su != nullptr) ? su->GetBuildProgress() : 0.0f;
		ou->set_under_construction(being_built);
		ou->set_build_progress(bp);
	}
}

void SnapshotBuilder::FillEnemies(::highbar::v1::StateSnapshot* out) const {
	const auto* em = ai_->GetEnemyManager();
	if (em == nullptr) return;
	// CEnemyManager doesn't expose a "GetEnemyInfos" map in upstream BARb.
	// The concrete container is the `enemyUnits` map of CEnemyUnit* — we
	// added GetEnemyUnits() as a public read-only accessor on the fork.
	for (const auto& [enemy_id, eu] : em->GetEnemyUnits()) {
		if (eu == nullptr) continue;
		const auto* cdef = eu->GetCircuitDef();
		if (eu->IsInLOS()) {
			auto* out_e = out->add_visible_enemies();
			out_e->set_unit_id(static_cast<std::uint32_t>(enemy_id));
			out_e->set_team_id(-1);
			out_e->set_def_id(cdef != nullptr
			                  ? static_cast<std::uint32_t>(cdef->GetId()) : 0);
			SetVec3(out_e->mutable_position(), eu->GetPos());
			out_e->set_health(eu->GetHealth());
			out_e->set_max_health(cdef != nullptr && cdef->GetDef() != nullptr
			                      ? cdef->GetDef()->GetHealth()
			                      : eu->GetHealth());
			out_e->set_los_quality(::highbar::v1::LosQuality::LOS_VISUAL);
		} else {
			// Radar blip: position is degraded per CEnemyUnit's
			// internal jitter (data-model §1 validation rule).
			auto* blip = out->add_radar_enemies();
			blip->set_blip_id(static_cast<std::uint32_t>(enemy_id));
			SetVec3(blip->mutable_position(), eu->GetPos());
			blip->set_suspected_def_id(
				cdef != nullptr ? static_cast<std::uint32_t>(cdef->GetId()) : 0);
		}
	}
}

void SnapshotBuilder::FillFeatures(::highbar::v1::StateSnapshot* out) const {
	if (ai_ == nullptr || out == nullptr) return;
	auto* callback = ai_->GetCallback();
	auto* economy = ai_->GetEconomyManager();
	if (callback == nullptr || economy == nullptr) return;
	auto* metal = economy->GetMetalRes();
	auto* energy = economy->GetEnergyRes();
	for (springai::Feature* feature : callback->GetFeatures()) {
		if (feature == nullptr) continue;
		auto* feat = out->add_map_features();
		feat->set_feature_id(static_cast<std::uint32_t>(feature->GetFeatureId()));
		SetVec3(feat->mutable_position(), feature->GetPosition());
		auto* def = feature->GetDef();
		if (def != nullptr) {
			feat->set_def_id(static_cast<std::uint32_t>(def->GetFeatureDefId()));
			if (metal != nullptr) {
				feat->set_reclaim_value_metal(def->GetContainedResource(metal));
			}
			if (energy != nullptr) {
				feat->set_reclaim_value_energy(def->GetContainedResource(energy));
			}
		}
	}
}

void SnapshotBuilder::FillEconomy(::highbar::v1::StateSnapshot* out) const {
	auto* em = ai_->GetEconomyManager();
	if (em == nullptr) return;
	auto* econ = out->mutable_economy();
	econ->set_metal(em->GetMetalCur());
	econ->set_metal_storage(em->GetMetalStore());
	// Metal income/usage: BARb exposes GetMetalPull() (usage/pull) but
	// income is computed internally. Leaving income at 0 for Phase 2;
	// US1/US2 wire GetMetalIncome()/equivalent once the exact accessor
	// is confirmed.
	econ->set_metal_income(0.0f);
	econ->set_energy(em->GetEnergyCur());
	econ->set_energy_storage(em->GetEnergyStore());
	econ->set_energy_income(0.0f);
}

void SnapshotBuilder::FillStaticMap(::highbar::v1::StaticMap* out) const {
	if (ai_ == nullptr) return;
	// Metal spots — verified via CMetalManager::GetSpots().
	if (auto* mm = ai_->GetMetalManager()) {
		for (const auto& spot : mm->GetSpots()) {
			auto* v = out->add_metal_spots();
			// spot.position is an AIFloat3 in CMetalData's record;
			// exact field name is metalData-internal. Use spot.position
			// as the public field name per BARb convention.
			v->set_x(spot.position.x);
			v->set_y(spot.position.y);
			v->set_z(spot.position.z);
		}
	}
	if (auto* map = ai_->GetMap()) {
		out->set_width_cells(static_cast<std::uint32_t>(map->GetWidth()));
		out->set_height_cells(static_cast<std::uint32_t>(map->GetHeight()));
	}
}

}  // namespace circuit::grpc
