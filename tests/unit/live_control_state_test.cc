// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/CommandQueue.h"
#include "grpc/LiveControlState.h"

#include <gtest/gtest.h>
#include <memory>

namespace {
using namespace circuit::grpc;
using namespace highbar::v1;

std::unique_ptr<LiveControlState> State() { return std::make_unique<LiveControlState>("p","proc","match","state","cmd","ctl"); }

LiveBinding Binding(std::uint64_t epoch) {
	LiveBinding b; b.set_plugin_id("p"); b.set_process_incarnation("proc");
	b.set_match_incarnation("match"); b.set_command_channel_incarnation("cmd");
	b.set_control_channel_incarnation("ctl"); b.set_broker_session_id("session");
	b.set_controller_id("controller"); b.set_controller_incarnation("controller-1");
	b.set_authority_epoch(epoch); b.set_module_sha256(std::string(32,'m')); b.set_module_generation(1);
	return b;
}

LiveControlAckReport Apply(LiveControlState& state, LiveControlDirectiveKind kind,
		std::uint64_t seq, std::uint32_t lease_ms=1000) {
	LiveControlDirective d; d.set_kind(kind); *d.mutable_binding()=Binding(1);
	d.set_control_sequence(seq); d.set_lease_duration_ms(lease_ms);
	return state.ApplyDirective(d, LiveControlState::Clock::time_point{});
}

TEST(LiveControlState, RevokeLinearizesWithoutEngineTickAndFencesSaturatedQueue) {
	auto state=State();
	EXPECT_EQ(Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1).disposition(), LIVE_CONTROL_ACK_RECORDED);
	CommandQueue queue(nullptr,2);
	QueuedCommand a,b; ASSERT_TRUE(queue.TryPush(std::move(a))); ASSERT_TRUE(queue.TryPush(std::move(b)));
	EXPECT_EQ(queue.Depth(),2u);
	EXPECT_EQ(Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_REVOKE,2,0).disposition(), LIVE_CONTROL_ACK_RECORDED);
	auto fence=state->CheckAuthority(Binding(1),LiveControlState::Clock::time_point{});
	EXPECT_FALSE(fence.ok); EXPECT_EQ(fence.reason,LIVE_FENCE_AUTHORITY_REVOKED);
	EXPECT_EQ(queue.Depth(),2u); // acknowledgment does not need drain/purge.
}

TEST(LiveControlState, LifetimesChangeOnRemovalButVisibilityDoesNotChangeIdentity) {
	auto state=State();
	const auto own1=state->MarkOwnedPresent(0); EXPECT_NE(own1,0u); EXPECT_EQ(state->OwnedLifetime(0),own1);
	state->MarkOwnedRemoved(0); EXPECT_EQ(state->OwnedLifetime(0),0u);
	const auto own2=state->MarkOwnedPresent(0); EXPECT_GT(own2,own1);
	const auto enemy1=state->MarkEnemyPresent(7,true); state->MarkEnemyVisual(7,false);
	EXPECT_EQ(state->EnemyLifetime(7),enemy1); EXPECT_FALSE(state->EnemyVisual(7));
	state->MarkEnemyVisual(7,true); EXPECT_EQ(state->EnemyLifetime(7),enemy1);
	state->MarkEnemyRemoved(7); const auto enemy2=state->MarkEnemyPresent(7,true); EXPECT_GT(enemy2,enemy1);
}

TEST(LiveControlState, ExactBasisAndUnitZeroLiveAdmissionArePreserved) {
	auto state=State(); Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto actor_life=state->MarkOwnedPresent(0);
	const auto basis=state->RecordBasis(9,30,123456,30);
	CommandQueue queue(nullptr,4);
	LiveCommandBatch live; *live.mutable_binding()=Binding(1); *live.mutable_basis()=basis;
	live.mutable_actor()->set_id(0); live.mutable_actor()->set_lifetime(actor_life);
	live.set_semantic_action(LIVE_SEMANTIC_ACTION_STOP);
	live.set_remaining_basis_validity_ms(500); live.set_remaining_command_lifetime_ms(500);
	live.set_remaining_lease_validity_ms(500);
	auto* batch=live.mutable_batch(); batch->set_batch_seq(1); batch->set_target_unit_id(0);
	batch->set_client_command_id(9007199254740993ULL); batch->add_commands()->mutable_stop()->set_unit_id(0);
	const auto result=AdmitLiveCommandBatch(queue,live,"live",*state,LiveControlState::Clock::time_point{});
	ASSERT_TRUE(result.accepted()); std::vector<QueuedCommand> drained; ASSERT_EQ(queue.Drain(&drained),1u);
	EXPECT_TRUE(drained[0].live); EXPECT_EQ(drained[0].authoritative_target_unit_id,0);
	EXPECT_EQ(drained[0].client_command_id,9007199254740993ULL);
	EXPECT_EQ(drained[0].live_actor.lifetime(),actor_life);
	auto altered=basis; altered.set_frame(31); EXPECT_FALSE(state->BasisKnown(altered));
}

TEST(LiveControlState, AppendIsExactlyShift32AndAttackRequiresTypedTarget) {
	auto state=State(); Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto actor=state->MarkOwnedPresent(0); const auto target=state->MarkEnemyPresent(1,true);
	const auto basis=state->RecordBasis(10,31,123457,30); CommandQueue queue(nullptr,4);
	auto base=[&]{ LiveCommandBatch l; *l.mutable_binding()=Binding(1); *l.mutable_basis()=basis;
		l.mutable_actor()->set_id(0); l.mutable_actor()->set_lifetime(actor);
		l.set_remaining_basis_validity_ms(500); l.set_remaining_command_lifetime_ms(500); l.set_remaining_lease_validity_ms(500);
		l.mutable_batch()->set_batch_seq(2); l.mutable_batch()->set_target_unit_id(0); l.mutable_batch()->set_client_command_id(2); return l; };
	auto append=base(); append.set_semantic_action(LIVE_SEMANTIC_ACTION_MOVE_APPEND);
	append.mutable_batch()->add_commands()->mutable_move_unit()->set_options(32);
	EXPECT_TRUE(AdmitLiveCommandBatch(queue,append,"live",*state,LiveControlState::Clock::time_point{}).accepted());
	auto bad=base(); bad.mutable_batch()->set_batch_seq(3); bad.mutable_batch()->set_client_command_id(3);
	bad.set_semantic_action(LIVE_SEMANTIC_ACTION_MOVE_APPEND); bad.mutable_batch()->add_commands()->mutable_move_unit()->set_options(1);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,bad,"live",*state,LiveControlState::Clock::time_point{}).accepted());
	auto attack=base(); attack.mutable_batch()->set_batch_seq(4); attack.mutable_batch()->set_client_command_id(4);
	attack.set_semantic_action(LIVE_SEMANTIC_ACTION_ATTACK_VISIBLE_UNIT);
	attack.mutable_visible_attack_target()->set_id(1); attack.mutable_visible_attack_target()->set_lifetime(target);
	attack.mutable_batch()->add_commands()->mutable_attack()->set_target_unit_id(1);
	EXPECT_TRUE(AdmitLiveCommandBatch(queue,attack,"live",*state,LiveControlState::Clock::time_point{}).accepted());
}

TEST(LiveControlState, DrainFenceUsesControlledClockAndCurrentActorLifetime) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	QueuedCommand q; q.live=true; q.live_binding=Binding(1); q.live_actor.set_id(0);
	q.live_actor.set_lifetime(state->MarkOwnedPresent(0));
	q.live_basis=state->RecordBasis(20,40,200000,30);
	q.live_basis_deadline=t0+std::chrono::milliseconds(500);
	q.live_command_deadline=t0+std::chrono::milliseconds(400);
	q.live_lease_deadline=t0+std::chrono::milliseconds(800);
	EXPECT_TRUE(state->CheckQueuedCommand(q,t0+std::chrono::milliseconds(399)).ok);
	auto expired=state->CheckQueuedCommand(q,t0+std::chrono::milliseconds(400));
	EXPECT_FALSE(expired.ok); EXPECT_EQ(expired.reason,LIVE_FENCE_COMMAND_EXPIRED);
	state->MarkOwnedRemoved(0);
	auto removed=state->CheckQueuedCommand(q,t0+std::chrono::milliseconds(100));
	EXPECT_FALSE(removed.ok); EXPECT_EQ(removed.reason,LIVE_FENCE_ACTOR_LIFETIME_CHANGED);
	state->MarkOwnedPresent(0);
	auto reused=state->CheckQueuedCommand(q,t0+std::chrono::milliseconds(100));
	EXPECT_FALSE(reused.ok); EXPECT_EQ(reused.reason,LIVE_FENCE_ACTOR_LIFETIME_CHANGED);
}

TEST(LiveControlState, AttackFenceRejectsVisibilityLossAndTargetReuse) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	QueuedCommand q; q.live=true; q.live_binding=Binding(1); q.live_semantic_action=LIVE_SEMANTIC_ACTION_ATTACK_VISIBLE_UNIT;
	q.live_actor.set_id(0); q.live_actor.set_lifetime(state->MarkOwnedPresent(0));
	q.live_attack_target.emplace(); q.live_attack_target->set_id(1);
	q.live_attack_target->set_lifetime(state->MarkEnemyPresent(1,true));
	q.live_basis=state->RecordBasis(21,41,200001,30);
	q.live_basis_deadline=q.live_command_deadline=q.live_lease_deadline=t0+std::chrono::seconds(1);
	EXPECT_TRUE(state->CheckQueuedCommand(q,t0).ok);
	state->MarkEnemyVisual(1,false);
	auto hidden=state->CheckQueuedCommand(q,t0);
	EXPECT_EQ(hidden.reason,LIVE_FENCE_TARGET_NOT_VISUAL);
	state->MarkEnemyVisual(1,true); state->MarkEnemyRemoved(1); state->MarkEnemyPresent(1,true);
	auto reused=state->CheckQueuedCommand(q,t0);
	EXPECT_EQ(reused.reason,LIVE_FENCE_TARGET_LIFETIME_CHANGED);
}

TEST(LiveControlState, RevocationCannotInterleaveWithGuardedDispatch) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	QueuedCommand q; q.live=true; q.live_binding=Binding(1); q.live_actor.set_id(0);
	q.live_actor.set_lifetime(state->MarkOwnedPresent(0)); q.live_basis=state->RecordBasis(22,42,200002,30);
	q.live_basis_deadline=q.live_command_deadline=q.live_lease_deadline=t0+std::chrono::seconds(1);
	bool called=false; auto result=state->DispatchGuarded(q,[&]{ called=true; return true; },t0);
	EXPECT_TRUE(result.ok); EXPECT_TRUE(called);
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_REVOKE,2,0);
	called=false; result=state->DispatchGuarded(q,[&]{ called=true; return true; },t0);
	EXPECT_FALSE(result.ok); EXPECT_FALSE(called); EXPECT_EQ(result.reason,LIVE_FENCE_AUTHORITY_REVOKED);
}
} // namespace
