// SPDX-License-Identifier: GPL-2.0-only
#include "grpc/CommandQueue.h"
#include "grpc/CommandDispatch.h"
#include "grpc/LiveControlState.h"
#include "grpc/TacticalNativeState.h"

#include <gtest/gtest.h>
#include <memory>

namespace {
using namespace circuit::grpc;
using namespace highbar::v1;

const std::string& Match() { static const std::string value(16, 'm'); return value; }
std::unique_ptr<LiveControlState> State() { return std::make_unique<LiveControlState>("p","proc",Match(),"state","cmd","ctl"); }

LiveBinding Binding(std::uint64_t epoch) {
	LiveBinding b; b.set_plugin_id("p"); b.set_process_incarnation("proc");
	b.set_match_incarnation(Match()); b.set_command_channel_incarnation("cmd");
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

TEST(LiveControlState, RuntimeMatchIncarnationsAreFreshOpaqueSixteenByteValues) {
	const auto first = LiveControlState::NewMatchIncarnation();
	const auto second = LiveControlState::NewMatchIncarnation();
	EXPECT_EQ(first.size(), 16u);
	EXPECT_EQ(second.size(), 16u);
	EXPECT_NE(first, second);
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
	const auto basis=state->RecordBasis(9,30,123456,30,std::chrono::milliseconds(500),LiveControlState::Clock::time_point{});
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
	EXPECT_EQ(AdmitLiveCommandBatch(queue,live,"live",*state,LiveControlState::Clock::time_point{}).status,
	          CommandBatchAdmissionStatus::kDuplicate);
	EXPECT_TRUE(drained[0].live); EXPECT_EQ(drained[0].authoritative_target_unit_id,0);
	EXPECT_EQ(drained[0].client_command_id,9007199254740993ULL);
	EXPECT_EQ(drained[0].live_actor.lifetime(),actor_life);
	auto altered=basis; altered.set_frame(31); EXPECT_FALSE(state->BasisKnown(altered));
}

TEST(LiveControlState, AppendIsExactlyShift32AndAttackRequiresTypedTarget) {
	auto state=State(); Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto actor=state->MarkOwnedPresent(0); const auto target=state->MarkEnemyPresent(1,true);
	const auto basis=state->RecordBasis(10,31,123457,30,std::chrono::milliseconds(500),LiveControlState::Clock::time_point{}); CommandQueue queue(nullptr,4);
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
	q.live_basis=state->RecordBasis(20,40,200000,30,std::chrono::milliseconds(500),t0);
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
	q.live_basis=state->RecordBasis(21,41,200001,30,std::chrono::milliseconds(500),t0);
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
	q.live_actor.set_lifetime(state->MarkOwnedPresent(0)); q.live_basis=state->RecordBasis(22,42,200002,30,std::chrono::milliseconds(500),t0);
	q.live_basis_deadline=q.live_command_deadline=q.live_lease_deadline=t0+std::chrono::seconds(1);
	bool called=false; auto result=state->DispatchGuarded(q,[&]{ called=true; return true; },t0);
	EXPECT_TRUE(result.ok); EXPECT_TRUE(called);
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_REVOKE,2,0);
	called=false; result=state->DispatchGuarded(q,[&]{ called=true; return true; },t0);
	EXPECT_FALSE(result.ok); EXPECT_FALSE(called); EXPECT_EQ(result.reason,LIVE_FENCE_AUTHORITY_REVOKED);
}

TEST(LiveControlState, NativeEmissionAgeCannotBeRenewedByDelayedTransport) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1,2000);
	const auto actor=state->MarkOwnedPresent(0);
	const auto basis=state->RecordBasis(30,60,300000,30,std::chrono::milliseconds(500),t0);
	auto live=LiveCommandBatch{}; *live.mutable_binding()=Binding(1); *live.mutable_basis()=basis;
	live.mutable_actor()->set_id(0); live.mutable_actor()->set_lifetime(actor);
	live.set_semantic_action(LIVE_SEMANTIC_ACTION_STOP);
	live.set_remaining_basis_validity_ms(500); live.set_remaining_command_lifetime_ms(1000);
	live.set_remaining_lease_validity_ms(1000);
	auto* batch=live.mutable_batch(); batch->set_batch_seq(30); batch->set_target_unit_id(0);
	batch->set_client_command_id(30); batch->add_commands()->mutable_stop()->set_unit_id(0);
	CommandQueue queue(nullptr,2);
	const auto delayed=t0+std::chrono::milliseconds(400);
	ASSERT_TRUE(AdmitLiveCommandBatch(queue,live,"live",*state,delayed).accepted());
	std::vector<QueuedCommand> drained; ASSERT_EQ(queue.Drain(&drained),1u);
	EXPECT_TRUE(state->CheckQueuedCommand(drained[0],t0+std::chrono::milliseconds(499)).ok);
	auto expired=state->CheckQueuedCommand(drained[0],t0+std::chrono::milliseconds(500));
	EXPECT_EQ(expired.reason,LIVE_FENCE_BASIS_EXPIRED);
	const auto fresh_basis=state->RecordBasis(31,61,300001,30,std::chrono::milliseconds(500),delayed);
	*live.mutable_basis()=fresh_basis; live.set_remaining_basis_validity_ms(50);
	live.mutable_batch()->set_batch_seq(31); live.mutable_batch()->set_client_command_id(31);
	ASSERT_TRUE(AdmitLiveCommandBatch(queue,live,"live",*state,delayed).accepted());
	drained.clear(); ASSERT_EQ(queue.Drain(&drained),1u);
	EXPECT_TRUE(state->CheckQueuedCommand(drained[0],t0+std::chrono::milliseconds(449)).ok);
	EXPECT_EQ(state->CheckQueuedCommand(drained[0],t0+std::chrono::milliseconds(450)).reason,
	          LIVE_FENCE_BASIS_EXPIRED);
	*live.mutable_basis()=basis; live.set_remaining_basis_validity_ms(500);
	live.mutable_batch()->set_batch_seq(32); live.mutable_batch()->set_client_command_id(32);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,
		t0+std::chrono::milliseconds(501)).accepted());
}

TEST(LiveControlState, LegacyQueuedWorkIsFencedWhenLiveSessionEngages) {
	auto state=State();
	EXPECT_TRUE(state->LegacyGameplayAllowed());
	CommandQueue queue(nullptr,2); QueuedCommand legacy;
	ASSERT_TRUE(queue.TryPush(std::move(legacy)));
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	EXPECT_FALSE(state->LegacyGameplayAllowed());
	std::vector<QueuedCommand> drained; ASSERT_EQ(queue.Drain(&drained),1u);
	int engine_calls=0;
	EXPECT_FALSE(state->DispatchLegacyGuarded([&]{ ++engine_calls; return true; }));
	EXPECT_EQ(engine_calls,0);
}

TEST(LiveControlState, TotalOwnedAndVisualMetadataOverflowRefusesAuthority) {
	auto state=std::make_unique<LiveControlState>("p","proc",Match(),"state","cmd","ctl",2);
	state->MarkOwnedPresent(0); state->MarkEnemyPresent(1,true); state->MarkEnemyPresent(2,true);
	EXPECT_FALSE(state->SnapshotUnitMetadata().has_value());
	EXPECT_EQ(Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1).disposition(),LIVE_CONTROL_ACK_REFUSED);
}

TEST(LiveControlState, ExpiredLeaseCannotBeRenewedAtSameEpoch) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	LiveControlDirective arm; arm.set_kind(LIVE_CONTROL_DIRECTIVE_KIND_ARM); *arm.mutable_binding()=Binding(1);
	arm.set_control_sequence(1); arm.set_lease_duration_ms(100);
	EXPECT_EQ(state->ApplyDirective(arm,t0).disposition(),LIVE_CONTROL_ACK_RECORDED);
	LiveControlDirective renew; renew.set_kind(LIVE_CONTROL_DIRECTIVE_KIND_RENEW); *renew.mutable_binding()=Binding(1);
	renew.set_control_sequence(2); renew.set_lease_duration_ms(100);
	EXPECT_EQ(state->ApplyDirective(renew,t0+std::chrono::milliseconds(100)).disposition(),LIVE_CONTROL_ACK_REFUSED);
	LiveControlDirective fresh; fresh.set_kind(LIVE_CONTROL_DIRECTIVE_KIND_ARM); *fresh.mutable_binding()=Binding(2);
	fresh.set_control_sequence(3); fresh.set_lease_duration_ms(100);
	EXPECT_EQ(state->ApplyDirective(fresh,t0+std::chrono::milliseconds(101)).disposition(),LIVE_CONTROL_ACK_RECORDED);
}

TEST(LiveControlState, ReplacedControlIncarnationCannotReuseAuthority) {
	auto state=State(); Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto old=Binding(1);
	state->ReplaceChannels("cmd-replacement","ctl-replacement");
	EXPECT_FALSE(state->CheckAuthority(old,LiveControlState::Clock::time_point{}).ok);
	LiveControlDirective stale; stale.set_kind(LIVE_CONTROL_DIRECTIVE_KIND_RENEW);
	*stale.mutable_binding()=old; stale.set_control_sequence(2); stale.set_lease_duration_ms(1000);
	EXPECT_EQ(state->ApplyDirective(stale,LiveControlState::Clock::time_point{}).disposition(),
	          LIVE_CONTROL_ACK_REFUSED);
	auto replacement=Binding(2); replacement.set_command_channel_incarnation("cmd-replacement");
	replacement.set_control_channel_incarnation("ctl-replacement");
	LiveControlDirective fresh; fresh.set_kind(LIVE_CONTROL_DIRECTIVE_KIND_ARM);
	*fresh.mutable_binding()=replacement; fresh.set_control_sequence(1); fresh.set_lease_duration_ms(1000);
	EXPECT_EQ(state->ApplyDirective(fresh,LiveControlState::Clock::time_point{}).disposition(),
	          LIVE_CONTROL_ACK_RECORDED);
}

TEST(LiveControlState, TacticalFeatureQueueAndDescriptorFencesAreRechecked) {
	auto state=State();
	const auto actor_lifetime=state->MarkOwnedPresent(0);
	state->RecordTacticalCatalogue("catalogue",7,true);
	TacticalSnapshotMetadata snapshot; snapshot.set_catalogue_id("catalogue"); snapshot.set_catalogue_revision(7);
	auto* actor=snapshot.add_actors(); actor->mutable_actor()->set_id(0); actor->mutable_actor()->set_lifetime(actor_lifetime); actor->set_descriptor_revision(9);
	auto* descriptor=actor->add_descriptors(); descriptor->set_kind(NATIVE_TACTICAL_DESCRIPTOR_RECLAIM_FEATURE);
	auto* queue=actor->add_queue(); queue->set_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); queue->set_revision(11); queue->set_complete(true);
	auto* feature=snapshot.add_features(); feature->mutable_reference()->set_id(0); feature->mutable_reference()->set_lifetime(55); feature->set_definition_id(3);
	state->RecordTacticalSnapshot(snapshot);
	LiveCommandBatch batch; batch.mutable_actor()->set_id(0); batch.mutable_actor()->set_lifetime(actor_lifetime);
	auto* tactical=batch.mutable_tactical_command(); tactical->set_catalogue_id("catalogue"); tactical->set_catalogue_revision(7);
	tactical->set_actor_descriptor_revision(9); tactical->set_queue_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); tactical->set_expected_queue_revision(11);
	tactical->mutable_reclaim_feature()->mutable_target()->set_id(0); tactical->mutable_reclaim_feature()->mutable_target()->set_lifetime(55);
	tactical->mutable_reclaim_feature()->set_queue_policy(NATIVE_QUEUE_POLICY_REPLACE);
	EXPECT_TRUE(state->CheckTacticalCommand(batch).ok);

	tactical->mutable_reclaim_feature()->mutable_target()->set_lifetime(56);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_FEATURE_LIFETIME_CHANGED);
	tactical->mutable_reclaim_feature()->mutable_target()->set_lifetime(55);
	snapshot.mutable_actors(0)->mutable_queue(0)->set_revision(12); state->RecordTacticalSnapshot(snapshot);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_QUEUE_CHANGED);
	snapshot.mutable_actors(0)->mutable_queue(0)->set_revision(11);
	snapshot.mutable_actors(0)->mutable_descriptors(0)->set_disabled(true); state->RecordTacticalSnapshot(snapshot);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_CAPABILITY_CHANGED);
}

TEST(LiveControlState, FeatureDestroyedBeforeDrainRefusesStaleReclaimAndFreshReusePasses) {
	auto state = State();
	const auto t0 = LiveControlState::Clock::time_point{};
	Apply(*state, LIVE_CONTROL_DIRECTIVE_KIND_ARM, 1);
	const auto actor_lifetime = state->MarkOwnedPresent(0);
	const auto basis = state->RecordBasis(
		90, 100, 9000, 1, std::chrono::milliseconds(500), t0);
	state->RecordTacticalCatalogue("catalogue", 17, true);

	FeatureLifetimeLedger ledger;
	const VisibleFeatureSample original{7, 70, 1.0f, 2.0f, 3.0f};
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(90, 1, {original}));
	const auto stale = ledger.Reference(7);
	ASSERT_TRUE(stale.has_value());

	TacticalSnapshotMetadata snapshot;
	snapshot.set_catalogue_id("catalogue");
	snapshot.set_catalogue_revision(17);
	auto* actor = snapshot.add_actors();
	actor->mutable_actor()->set_id(0);
	actor->mutable_actor()->set_lifetime(actor_lifetime);
	actor->set_descriptor_revision(19);
	actor->add_descriptors()->set_kind(NATIVE_TACTICAL_DESCRIPTOR_RECLAIM_FEATURE);
	auto* queue = actor->add_queue();
	queue->set_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER);
	queue->set_revision(23);
	queue->set_complete(true);
	auto* feature = snapshot.add_features();
	feature->mutable_reference()->set_id(stale->id);
	feature->mutable_reference()->set_lifetime(stale->lifetime);
	feature->set_definition_id(stale->def_id);
	state->RecordTacticalSnapshot(snapshot);

	QueuedCommand queued;
	queued.live = true;
	queued.live_binding = Binding(1);
	queued.live_basis = basis;
	queued.live_actor.set_id(0);
	queued.live_actor.set_lifetime(actor_lifetime);
	queued.live_semantic_action = LIVE_SEMANTIC_ACTION_RECLAIM_FEATURE;
	queued.live_basis_deadline = queued.live_command_deadline
		= queued.live_lease_deadline = t0 + std::chrono::milliseconds(500);
	queued.live_tactical_command.emplace();
	auto& tactical = *queued.live_tactical_command;
	tactical.set_catalogue_id("catalogue");
	tactical.set_catalogue_revision(17);
	tactical.set_actor_descriptor_revision(19);
	tactical.set_queue_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER);
	tactical.set_expected_queue_revision(23);
	tactical.mutable_reclaim_feature()->mutable_target()->set_id(stale->id);
	tactical.mutable_reclaim_feature()->mutable_target()->set_lifetime(stale->lifetime);
	tactical.mutable_reclaim_feature()->set_queue_policy(NATIVE_QUEUE_POLICY_REPLACE);
	ASSERT_TRUE(state->CheckQueuedCommand(queued, t0).ok);

	ledger.MarkDestroyed(7);
	// The cached tactical snapshot still admits the queued wire reference. The
	// engine-thread ledger fence must therefore be the refusing boundary.
	ASSERT_TRUE(state->CheckQueuedCommand(queued, t0).ok);
	int engine_calls = 0;
	auto result = state->DispatchGuarded(queued, [&] {
		return DispatchCurrentFeatureReclaim(
			&ledger, *stale, original, [&] { ++engine_calls; });
	}, t0);
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.reason, LIVE_FENCE_CAPABILITY_CHANGED);
	EXPECT_EQ(engine_calls, 0);

	const VisibleFeatureSample reused{7, 71, 4.0f, 5.0f, 6.0f};
	ASSERT_TRUE(ledger.ReplaceBoundedCompleteVisibleSnapshot(91, 1, {reused}));
	const auto fresh = ledger.Reference(7);
	ASSERT_TRUE(fresh.has_value());
	EXPECT_NE(fresh->lifetime, stale->lifetime);
	snapshot.mutable_features(0)->mutable_reference()->set_lifetime(fresh->lifetime);
	snapshot.mutable_features(0)->set_definition_id(fresh->def_id);
	state->RecordTacticalSnapshot(snapshot);
	tactical.mutable_reclaim_feature()->mutable_target()->set_lifetime(fresh->lifetime);
	result = state->DispatchGuarded(queued, [&] {
		return DispatchCurrentFeatureReclaim(
			&ledger, *fresh, reused, [&] { ++engine_calls; });
	}, t0);
	EXPECT_TRUE(result.ok);
	EXPECT_EQ(engine_calls, 1);
}

TEST(LiveControlState, TacticalBuildDefinitionBusyPolicyAndRallyFailClosed) {
	auto state=State(); const auto lifetime=state->MarkOwnedPresent(0);
	state->RecordTacticalCatalogue("catalogue",8,true);
	TacticalSnapshotMetadata snapshot; snapshot.set_catalogue_id("catalogue"); snapshot.set_catalogue_revision(8);
	auto* actor=snapshot.add_actors(); actor->mutable_actor()->set_id(0); actor->mutable_actor()->set_lifetime(lifetime); actor->set_descriptor_revision(10);
	auto* build=actor->add_descriptors(); build->set_kind(NATIVE_TACTICAL_DESCRIPTOR_BUILD); build->add_allowed_definition_ids(42);
	auto* queue=actor->add_queue(); queue->set_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); queue->set_revision(12); queue->set_complete(true); queue->add_entries()->set_native_tag(5);
	state->RecordTacticalSnapshot(snapshot);
	LiveCommandBatch batch; batch.mutable_actor()->set_id(0); batch.mutable_actor()->set_lifetime(lifetime);
	auto* tactical=batch.mutable_tactical_command(); tactical->set_catalogue_id("catalogue"); tactical->set_catalogue_revision(8); tactical->set_actor_descriptor_revision(10); tactical->set_queue_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); tactical->set_expected_queue_revision(12);
	auto* intent=tactical->mutable_build(); intent->set_definition_id(42); intent->mutable_position()->set_x(10); intent->mutable_position()->set_z(20); intent->set_facing(NATIVE_BUILD_FACING_SOUTH); intent->set_queue_policy(NATIVE_QUEUE_POLICY_REJECT_IF_BUSY);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_PARAMETER_REFUSED);
	intent->set_queue_policy(NATIVE_QUEUE_POLICY_REPLACE); EXPECT_TRUE(state->CheckTacticalCommand(batch).ok);
	intent->set_definition_id(43); EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_PARAMETER_REFUSED);

	tactical->mutable_set_rally()->mutable_position()->set_x(1);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_CAPABILITY_CHANGED);
	state->RecordTacticalCatalogue("catalogue",8,false);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_CATALOGUE_INCOMPLETE);
}

TEST(LiveControlState, MatchedRallyQueueRequiresCompleteExactRevisionAtFinalFence) {
	auto state=State(); const auto lifetime=state->MarkOwnedPresent(4);
	state->RecordTacticalCatalogue("catalogue",81,true);
	TacticalSnapshotMetadata snapshot; snapshot.set_catalogue_id("catalogue"); snapshot.set_catalogue_revision(81);
	auto* actor=snapshot.add_actors(); actor->mutable_actor()->set_id(4);
	actor->mutable_actor()->set_lifetime(lifetime); actor->set_descriptor_revision(82);
	actor->add_descriptors()->set_kind(NATIVE_TACTICAL_DESCRIPTOR_SET_RALLY);
	auto* rally=actor->add_queue(); rally->set_domain(NATIVE_QUEUE_DOMAIN_FACTORY_RALLY);
	rally->set_revision(83); rally->set_complete(true);
	state->RecordTacticalSnapshot(snapshot);
	LiveCommandBatch batch; batch.mutable_actor()->set_id(4); batch.mutable_actor()->set_lifetime(lifetime);
	auto* tactical=batch.mutable_tactical_command(); tactical->set_catalogue_id("catalogue");
	tactical->set_catalogue_revision(81); tactical->set_actor_descriptor_revision(82);
	tactical->set_queue_domain(NATIVE_QUEUE_DOMAIN_FACTORY_RALLY);
	tactical->set_expected_queue_revision(83);
	tactical->mutable_set_rally()->mutable_position()->set_x(100);
	tactical->mutable_set_rally()->mutable_position()->set_z(200);
	EXPECT_TRUE(state->CheckTacticalCommand(batch).ok);

	tactical->set_expected_queue_revision(84);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_QUEUE_CHANGED);
	tactical->set_expected_queue_revision(83);
	snapshot.mutable_actors(0)->mutable_queue(0)->set_complete(false);
	state->RecordTacticalSnapshot(snapshot);
	EXPECT_EQ(state->CheckTacticalCommand(batch).reason,LIVE_FENCE_QUEUE_CHANGED);
}

TEST(LiveControlState, TacticalAdmissionRequiresExactLegacyFacingPositionAndOptions) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto lifetime=state->MarkOwnedPresent(0);
	const auto basis=state->RecordBasis(80,100,8000,1,std::chrono::milliseconds(500),t0);
	state->RecordTacticalCatalogue("catalogue",9,true);
	TacticalSnapshotMetadata snapshot; snapshot.set_catalogue_id("catalogue"); snapshot.set_catalogue_revision(9);
	auto* actor=snapshot.add_actors(); actor->mutable_actor()->set_id(0); actor->mutable_actor()->set_lifetime(lifetime); actor->set_descriptor_revision(13);
	auto* descriptor=actor->add_descriptors(); descriptor->set_kind(NATIVE_TACTICAL_DESCRIPTOR_BUILD); descriptor->add_allowed_definition_ids(42);
	auto* observed=actor->add_queue(); observed->set_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); observed->set_revision(14); observed->set_complete(true);
	state->RecordTacticalSnapshot(snapshot);
	LiveCommandBatch live; *live.mutable_binding()=Binding(1); *live.mutable_basis()=basis;
	live.mutable_actor()->set_id(0); live.mutable_actor()->set_lifetime(lifetime); live.set_semantic_action(LIVE_SEMANTIC_ACTION_BUILD);
	live.set_remaining_basis_validity_ms(500); live.set_remaining_command_lifetime_ms(500); live.set_remaining_lease_validity_ms(500);
	auto* tactical=live.mutable_tactical_command(); tactical->set_catalogue_id("catalogue"); tactical->set_catalogue_revision(9); tactical->set_actor_descriptor_revision(13); tactical->set_queue_domain(NATIVE_QUEUE_DOMAIN_ACTOR_ORDER); tactical->set_expected_queue_revision(14);
	auto* intent=tactical->mutable_build(); intent->set_definition_id(42); intent->mutable_position()->set_x(100); intent->mutable_position()->set_elevation(5); intent->mutable_position()->set_z(200); intent->set_queue_policy(NATIVE_QUEUE_POLICY_APPEND);
	auto* batch=live.mutable_batch(); batch->set_batch_seq(1); batch->set_client_command_id(1); batch->set_target_unit_id(0);
	auto* legacy=batch->add_commands()->mutable_build_unit(); legacy->set_unit_id(0); legacy->set_to_build_unit_def_id(42); legacy->set_options(32); legacy->mutable_build_position()->set_x(100); legacy->mutable_build_position()->set_y(5); legacy->mutable_build_position()->set_z(200);
	CommandQueue queue(nullptr,8);
	const std::pair<NativeBuildFacing, int> facings[] = {
		{NATIVE_BUILD_FACING_NORTH, 2},
		{NATIVE_BUILD_FACING_EAST, 1},
		{NATIVE_BUILD_FACING_SOUTH, 0},
		{NATIVE_BUILD_FACING_WEST, 3},
	};
	std::uint64_t sequence = 1;
	for (const auto& [protocol, engine] : facings) {
		intent->set_facing(protocol); legacy->set_facing(engine);
		batch->set_batch_seq(sequence); batch->set_client_command_id(sequence++);
		EXPECT_TRUE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	}
	intent->set_facing(static_cast<NativeBuildFacing>(99)); legacy->set_facing(-1);
	batch->set_batch_seq(sequence); batch->set_client_command_id(sequence++);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	intent->set_facing(NATIVE_BUILD_FACING_SOUTH); legacy->set_facing(2);
	batch->set_batch_seq(sequence); batch->set_client_command_id(sequence++);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	legacy->set_facing(0); legacy->mutable_build_position()->set_x(101);
	batch->set_batch_seq(sequence); batch->set_client_command_id(sequence++);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	legacy->mutable_build_position()->set_x(100); legacy->set_options(0);
	batch->set_batch_seq(sequence); batch->set_client_command_id(sequence);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
}

TEST(LiveControlState, RallyAdmissionRequiresExactLegacyMoveAndFreshRallyQueue) {
	auto state=State(); const auto t0=LiveControlState::Clock::time_point{};
	Apply(*state,LIVE_CONTROL_DIRECTIVE_KIND_ARM,1);
	const auto lifetime=state->MarkOwnedPresent(4);
	const auto basis=state->RecordBasis(90,110,9000,1,std::chrono::milliseconds(500),t0);
	state->RecordTacticalCatalogue("catalogue",91,true);
	TacticalSnapshotMetadata snapshot; snapshot.set_catalogue_id("catalogue"); snapshot.set_catalogue_revision(91);
	auto* actor=snapshot.add_actors(); actor->mutable_actor()->set_id(4);
	actor->mutable_actor()->set_lifetime(lifetime); actor->set_descriptor_revision(92);
	actor->add_descriptors()->set_kind(NATIVE_TACTICAL_DESCRIPTOR_SET_RALLY);
	auto* observed=actor->add_queue(); observed->set_domain(NATIVE_QUEUE_DOMAIN_FACTORY_RALLY);
	observed->set_revision(93); observed->set_complete(true); state->RecordTacticalSnapshot(snapshot);
	LiveCommandBatch live; *live.mutable_binding()=Binding(1); *live.mutable_basis()=basis;
	live.mutable_actor()->set_id(4); live.mutable_actor()->set_lifetime(lifetime);
	live.set_semantic_action(LIVE_SEMANTIC_ACTION_SET_RALLY);
	live.set_remaining_basis_validity_ms(500); live.set_remaining_command_lifetime_ms(500);
	live.set_remaining_lease_validity_ms(500);
	auto* tactical=live.mutable_tactical_command(); tactical->set_catalogue_id("catalogue");
	tactical->set_catalogue_revision(91); tactical->set_actor_descriptor_revision(92);
	tactical->set_queue_domain(NATIVE_QUEUE_DOMAIN_FACTORY_RALLY);
	tactical->set_expected_queue_revision(93);
	tactical->mutable_set_rally()->mutable_position()->set_x(100);
	tactical->mutable_set_rally()->mutable_position()->set_elevation(5);
	tactical->mutable_set_rally()->mutable_position()->set_z(200);
	auto* batch=live.mutable_batch(); batch->set_batch_seq(1); batch->set_client_command_id(1);
	batch->set_target_unit_id(4); auto* move=batch->add_commands()->mutable_move_unit();
	move->set_unit_id(4); move->set_options(0); move->mutable_to_position()->set_x(100);
	move->mutable_to_position()->set_y(5); move->mutable_to_position()->set_z(200);
	CommandQueue queue(nullptr,3);
	EXPECT_TRUE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	live.mutable_batch()->set_batch_seq(2); live.mutable_batch()->set_client_command_id(2);
	live.mutable_batch()->mutable_commands(0)->mutable_move_unit()->set_options(32);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
	live.mutable_batch()->set_batch_seq(3); live.mutable_batch()->set_client_command_id(3);
	live.mutable_batch()->mutable_commands(0)->mutable_move_unit()->set_options(0);
	live.mutable_tactical_command()->set_expected_queue_revision(94);
	EXPECT_FALSE(AdmitLiveCommandBatch(queue,live,"live",*state,t0).accepted());
}
} // namespace
