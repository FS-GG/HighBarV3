#!/usr/bin/env python3
"""HighBarCoordinator — client-mode relay.

Hosts TWO services on the same gRPC server:

  1. HighBarCoordinator (the plugin dials in via this)
     - Heartbeat       : unary
     - PushState       : plugin streams StateUpdates here
     - OpenCommandChannel: coord streams CommandBatches to plugin

  2. HighBarProxy (external observers / AI-role clients dial in via this)
     - Hello           : unary handshake
     - StreamState     : observer gets relayed StateUpdates from the
                         plugin's PushState feed (fan-out)
     - SubmitCommands  : AI-role client streams CommandBatches;
                         coordinator forwards them to the plugin via
                         the OpenCommandChannel stream
     - Others          : UNIMPLEMENTED (future phases)

This is the "flip" — server-mode HighBarProxy now lives in the
coordinator instead of the plugin. The existing F# / Python clients
written against HighBarProxy work unchanged; they just connect to the
coordinator endpoint instead of the plugin's UDS.
"""
import sys
import os
import time
import queue
import threading
import argparse
import math

sys.path.insert(0, os.environ.get("HIGHBAR_PYPROTO_DIR", "/tmp/hb-run/pyproto"))

import grpc
from concurrent import futures
from google.protobuf.descriptor import FieldDescriptor
from highbar import callbacks_pb2
from highbar import coordinator_pb2, coordinator_pb2_grpc
from highbar import commands_pb2, service_pb2, service_pb2_grpc
from highbar import state_pb2

TOKEN_HEADER = "x-highbar-ai-token"


class Relay:
    """Thread-safe hub connecting plugin feeds to external clients."""

    def __init__(self, result_mode="required", result_timeout=5.0):
        # State broadcast: PushState pushes here, StreamState subscribers read.
        # Per-subscriber Queue, unbounded (best-effort; real impl would bound).
        self._state_subs_lock = threading.Lock()
        self._state_subs = []  # list[queue.Queue[state_pb2.StateUpdate]]
        self._latest_snapshot = None
        # Exactly one owning plugin command channel. Pending native admission
        # is keyed by channel incarnation plus the full uint64 wire identity.
        self.cmd_forward = queue.Queue()
        self._cmd_channel_lock = threading.Lock()
        self._command_owner = None
        self._pending = {}
        self._completed = {}
        self._completed_order = []
        self.result_mode = result_mode
        self.result_timeout = result_timeout
        self.state_updates_received = 0
        self.max_seq_seen = 0
        self.commands_relayed = 0

    def add_state_subscriber(self):
        q = queue.Queue(maxsize=8192)
        with self._state_subs_lock:
            self._state_subs.append(q)
            if self._latest_snapshot is not None:
                try:
                    q.put_nowait(self._latest_snapshot)
                except queue.Full:
                    pass
        return q

    def remove_state_subscriber(self, q):
        with self._state_subs_lock:
            try:
                self._state_subs.remove(q)
            except ValueError:
                pass

    def publish_state(self, update):
        self.state_updates_received += 1
        if update.seq > self.max_seq_seen:
            self.max_seq_seen = update.seq
        with self._state_subs_lock:
            if update.HasField("snapshot"):
                self._latest_snapshot = update
            for q in self._state_subs:
                try:
                    q.put_nowait(update)
                except queue.Full:
                    pass  # slow subscriber; drop

    def activate_command_channel(self, plugin_id, incarnation, protocol):
        with self._cmd_channel_lock:
            if self._command_owner is not None:
                raise RuntimeError("an owning plugin command channel is already active")
            correlated = (
                protocol == coordinator_pb2.ADMISSION_RESULT_PROTOCOL_CORRELATED_V1
                and bool(incarnation)
            )
            if self.result_mode == "required" and not correlated:
                raise RuntimeError("native correlated admission results are required")
            self._command_owner = (plugin_id, incarnation, correlated)

    def deactivate_command_channel(self, plugin_id, incarnation):
        with self._cmd_channel_lock:
            if (self._command_owner is None
                    or self._command_owner[:2] != (plugin_id, incarnation)):
                return
            self._command_owner = None
            for key, pending in list(self._pending.items()):
                if key[0] == incarnation:
                    pending["error"] = "owning plugin channel disconnected after forwarding"
                    pending["event"].set()
            self._clear_forwarded_commands_locked()

    def has_active_command_channel(self):
        with self._cmd_channel_lock:
            return self._command_owner is not None

    def _clear_forwarded_commands_locked(self):
        while True:
            try:
                self.cmd_forward.get_nowait()
            except queue.Empty:
                return

    def register_and_forward(self, batch):
        with self._cmd_channel_lock:
            if self._command_owner is None:
                raise RuntimeError("plugin command channel is not connected")
            _, incarnation, correlated = self._command_owner
            if self.result_mode == "required" and not correlated:
                raise RuntimeError("owning plugin is observation-only")
            if not correlated:
                self.cmd_forward.put((incarnation, batch))
                self.commands_relayed += 1
                return None
            key = (incarnation, batch.batch_seq, batch.client_command_id)
            if key in self._pending or key in self._completed:
                raise ValueError("duplicate pending command identity")
            pending = {"event": threading.Event(), "result": None, "error": None}
            # Registration precedes the queue write while holding the owner
            # lock, so even an immediate native report finds its waiter.
            self._pending[key] = pending
            self.cmd_forward.put((incarnation, batch))
            self.commands_relayed += 1
            return key, pending

    def complete_result(self, plugin_id, incarnation, result):
        key = (incarnation, result.batch_seq, result.client_command_id)
        with self._cmd_channel_lock:
            if (self._command_owner is None
                    or self._command_owner[:2] != (plugin_id, incarnation)):
                raise PermissionError("result is not from the owning plugin incarnation")
            pending = self._pending.get(key)
            if pending is None:
                return (coordinator_pb2.COMMAND_BATCH_RESULT_DUPLICATE
                        if key in self._completed
                        else coordinator_pb2.COMMAND_BATCH_RESULT_LATE)
            if pending["result"] is not None:
                return coordinator_pb2.COMMAND_BATCH_RESULT_DUPLICATE
            pending["result"] = commands_pb2.CommandBatchResult()
            pending["result"].CopyFrom(result)
            pending["event"].set()
            return coordinator_pb2.COMMAND_BATCH_RESULT_RECORDED

    def await_result(self, key, pending, context):
        deadline = time.monotonic() + self.result_timeout
        while not pending["event"].wait(timeout=min(0.05, max(0, deadline - time.monotonic()))):
            if not context.is_active():
                self._forget_pending(key)
                raise RuntimeError("submitter cancelled after command forwarding")
            if time.monotonic() >= deadline:
                self._forget_pending(key)
                raise TimeoutError("native admission result timed out after command forwarding")
        with self._cmd_channel_lock:
            self._pending.pop(key, None)
            if pending["result"] is not None:
                self._completed[key] = pending["result"].SerializeToString()
                self._completed_order.append(key)
                while len(self._completed_order) > 4096:
                    old = self._completed_order.pop(0)
                    self._completed.pop(old, None)
        if pending["error"] is not None:
            raise RuntimeError(pending["error"])
        return pending["result"]

    def _forget_pending(self, key):
        with self._cmd_channel_lock:
            self._pending.pop(key, None)


def _validate_finite_fields(message, path):
    for field, value in message.ListFields():
        field_path = f"{path}.{field.name}" if path else field.name
        if field.is_repeated:
            if field.type == FieldDescriptor.TYPE_MESSAGE:
                for idx, item in enumerate(value):
                    err = _validate_finite_fields(item, f"{field_path}[{idx}]")
                    if err is not None:
                        return err
            elif field.type in (FieldDescriptor.TYPE_FLOAT,
                                FieldDescriptor.TYPE_DOUBLE):
                for idx, item in enumerate(value):
                    if not math.isfinite(item):
                        return f"{field_path}[{idx}] must be finite"
            continue

        if field.type == FieldDescriptor.TYPE_MESSAGE:
            err = _validate_finite_fields(value, field_path)
            if err is not None:
                return err
        elif field.type in (FieldDescriptor.TYPE_FLOAT,
                            FieldDescriptor.TYPE_DOUBLE):
            if not math.isfinite(value):
                return f"{field_path} must be finite"

    return None


def validate_command_batch(batch):
    if batch.batch_seq <= 0:
        return "batch_seq must be > 0"
    if len(batch.commands) == 0:
        return "commands must not be empty"
    for idx, command in enumerate(batch.commands):
        if command.WhichOneof("command") is None:
            return f"commands[{idx}] command must be set"
    err = _validate_finite_fields(batch, "batch")
    if err is not None:
        return err
    return None


# ----------------------------------------------------------------------
# HighBarCoordinator service (plugin-facing)
# ----------------------------------------------------------------------

class CoordSvc(coordinator_pb2_grpc.HighBarCoordinatorServicer):
    def __init__(self, coord_id, relay):
        self.coord_id = coord_id
        self.relay = relay
        self.heartbeats = 0

    def Heartbeat(self, request, context):
        self.heartbeats += 1
        if self.heartbeats % 50 == 1:
            print(f"[hb={self.heartbeats:04d}] plugin={request.plugin_id} "
                  f"frame={request.frame}", flush=True)
        return coordinator_pb2.HeartbeatResponse(
            coordinator_id=self.coord_id,
            echoed_frame=request.frame,
            schema_version="1.0.0",
        )

    def PushState(self, request_iterator, context):
        peer = context.peer()
        print(f"[push] plugin stream opened peer={peer}", flush=True)
        for update in request_iterator:
            self.relay.publish_state(update)
        print(f"[push] plugin stream closed peer={peer} "
              f"total_seen={self.relay.state_updates_received}", flush=True)
        return coordinator_pb2.PushAck(
            messages_received=self.relay.state_updates_received,
            max_seq_seen=self.relay.max_seq_seen,
            coordinator_id=self.coord_id,
        )

    def OpenCommandChannel(self, request, context):
        print(f"[cmd-ch] plugin={request.plugin_id} subscribed "
              f"peer={context.peer()}", flush=True)
        if request.schema_version != "1.0.0":
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "command channel schema mismatch")
        try:
            self.relay.activate_command_channel(
                request.plugin_id,
                request.channel_incarnation,
                request.admission_result_protocol,
            )
        except RuntimeError as exc:
            context.abort(grpc.StatusCode.ALREADY_EXISTS, str(exc))
        disconnected = threading.Event()
        context.add_callback(disconnected.set)
        # Serve forwarded commands from the central queue until the
        # plugin disconnects.
        try:
            while not disconnected.is_set():
                try:
                    incarnation, batch = self.relay.cmd_forward.get(timeout=0.5)
                    if incarnation != request.channel_incarnation:
                        continue
                    command_kinds = [
                        command.WhichOneof("command") or "unset"
                        for command in batch.commands
                    ]
                    print(f"[cmd-ch] forwarding batch "
                          f"seq={batch.batch_seq} "
                          f"ncmds={len(batch.commands)} "
                          f"kinds={command_kinds}", flush=True)
                    yield batch
                except queue.Empty:
                    continue
        except grpc.RpcError:
            pass
        finally:
            self.relay.deactivate_command_channel(
                request.plugin_id, request.channel_incarnation)
        print(f"[cmd-ch] plugin={request.plugin_id} disconnected",
              flush=True)

    def ReportCommandBatchResult(self, request, context):
        if request.schema_version != "1.0.0":
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "admission result schema mismatch")
        try:
            disposition = self.relay.complete_result(
                request.plugin_id,
                request.channel_incarnation,
                request.result,
            )
        except PermissionError as exc:
            context.abort(grpc.StatusCode.PERMISSION_DENIED, str(exc))
        return coordinator_pb2.CommandBatchResultReportAck(
            disposition=disposition)


# ----------------------------------------------------------------------
# HighBarProxy service (external-client-facing; the "flip")
# ----------------------------------------------------------------------

class ProxySvc(service_pb2_grpc.HighBarProxyServicer):
    def __init__(self, coord_id, relay):
        self.coord_id = coord_id
        self.relay = relay
        self.session_seq = 0
        self.callback_proxy_endpoint = os.environ.get(
            "HIGHBAR_CALLBACK_PROXY_ENDPOINT", ""
        ).strip()
        self.callback_proxy_token_file = (
            os.environ.get("HIGHBAR_CALLBACK_PROXY_TOKEN_FILE", "").strip()
            or os.environ.get("HIGHBAR_TOKEN_PATH", "").strip()
        )

    def _new_session_id(self):
        self.session_seq += 1
        return f"{self.coord_id}-sess-{self.session_seq}"

    def _token_metadata(self, context):
        return [
            (item.key, item.value)
            for item in context.invocation_metadata()
            if item.key == TOKEN_HEADER
        ]

    def _callback_proxy_metadata(self, context):
        if self.callback_proxy_token_file:
            try:
                with open(
                    self.callback_proxy_token_file,
                    "r",
                    encoding="utf-8",
                ) as handle:
                    token = handle.read().strip()
                if token:
                    return [(TOKEN_HEADER, token)], "proxy-token-file"
            except OSError as exc:
                print(
                    f"[proxy] callback token file unavailable "
                    f"path={self.callback_proxy_token_file}: {exc}",
                    flush=True,
                )
        metadata = self._token_metadata(context)
        return metadata, "inbound-metadata" if metadata else "absent"

    def Hello(self, request, context):
        print(f"[proxy] Hello from {context.peer()} "
              f"schema={request.schema_version}", flush=True)
        if request.schema_version != "1.0.0":
            context.set_code(grpc.StatusCode.FAILED_PRECONDITION)
            context.set_details(
                f"schema mismatch: server=1.0.0 client={request.schema_version}")
            return service_pb2.HelloResponse()
        static_map = None
        current_frame = 0
        if self.callback_proxy_endpoint:
            metadata = self._token_metadata(context)
            try:
                with grpc.insecure_channel(self.callback_proxy_endpoint) as channel:
                    stub = service_pb2_grpc.HighBarProxyStub(channel)
                    downstream = stub.Hello(
                        service_pb2.HelloRequest(
                            schema_version=request.schema_version,
                            client_id=request.client_id,
                            role=request.role,
                        ),
                        metadata=metadata,
                        timeout=5.0,
                    )
                static_map = downstream.static_map
                current_frame = downstream.current_frame
            except grpc.RpcError:
                static_map = None
                current_frame = 0
        response = service_pb2.HelloResponse(
            schema_version="1.0.0",
            session_id=self._new_session_id(),
            current_frame=current_frame,
        )
        if static_map is not None:
            response.static_map.CopyFrom(static_map)
        return response

    def StreamState(self, request, context):
        print(f"[proxy] StreamState subscriber from {context.peer()} "
              f"resume_from_seq={request.resume_from_seq}", flush=True)
        q = self.relay.add_state_subscriber()
        try:
            while context.is_active():
                try:
                    update = q.get(timeout=0.5)
                    yield update
                except queue.Empty:
                    continue
        finally:
            self.relay.remove_state_subscriber(q)
            print(f"[proxy] StreamState subscriber from {context.peer()} "
                  f"disconnected", flush=True)

    def SubmitCommands(self, request_iterator, context):
        results = []
        forwarded = 0
        for batch in request_iterator:
            if forwarded >= 256:
                context.abort(grpc.StatusCode.RESOURCE_EXHAUSTED,
                              "SubmitCommands is limited to 256 batches")
            err = validate_command_batch(batch)
            if err is not None:
                print(f"[proxy] SubmitCommands invalid from {context.peer()}: "
                      f"{err}", flush=True)
                context.abort(grpc.StatusCode.INVALID_ARGUMENT, err)
            try:
                registered = self.relay.register_and_forward(batch)
            except ValueError as exc:
                context.abort(grpc.StatusCode.ALREADY_EXISTS, str(exc))
            except RuntimeError as exc:
                context.abort(grpc.StatusCode.UNAVAILABLE, str(exc))
            forwarded += 1
            if registered is not None:
                try:
                    results.append(self.relay.await_result(*registered, context))
                except TimeoutError as exc:
                    context.abort(grpc.StatusCode.DEADLINE_EXCEEDED, str(exc))
                except RuntimeError as exc:
                    context.abort(grpc.StatusCode.UNAVAILABLE, str(exc))
        print(f"[proxy] SubmitCommands from {context.peer()}: "
              f"received {forwarded} batches, native_results={len(results)}",
              flush=True)
        ack = service_pb2.CommandAck()
        ack.results.extend(results)
        for result in results:
            if result.status in (
                    commands_pb2.COMMAND_BATCH_ACCEPTED,
                    commands_pb2.COMMAND_BATCH_ACCEPTED_WITH_WARNINGS):
                ack.batches_accepted += 1
                ack.last_accepted_batch_seq = result.batch_seq
            elif result.status == commands_pb2.COMMAND_BATCH_REJECTED_QUEUE_FULL:
                ack.batches_rejected_full += 1
            else:
                ack.batches_rejected_invalid += 1
        # Explicit legacy mode remains observation-only: forwarding is visible
        # in logs, while an empty result list never claims native acceptance.
        return ack

    def InvokeCallback(self, request, context):
        if not self.callback_proxy_endpoint:
            context.abort(
                grpc.StatusCode.FAILED_PRECONDITION,
                "InvokeCallback relay unavailable; set HIGHBAR_CALLBACK_PROXY_ENDPOINT",
            )
        metadata, token_source = self._callback_proxy_metadata(context)
        print(
            f"[proxy] InvokeCallback from {context.peer()} "
            f"callback_id={request.callback_id} "
            f"endpoint={self.callback_proxy_endpoint} "
            f"token={token_source}",
            flush=True,
        )
        try:
            with grpc.insecure_channel(self.callback_proxy_endpoint) as channel:
                stub = service_pb2_grpc.HighBarProxyStub(channel)
                stub.Hello(
                    service_pb2.HelloRequest(
                        schema_version="1.0.0",
                        client_id=f"{self.coord_id}-callback-relay",
                        role=service_pb2.Role.ROLE_AI,
                    ),
                    metadata=metadata,
                    timeout=5.0,
                )
                return stub.InvokeCallback(request, metadata=metadata, timeout=5.0)
        except grpc.RpcError as exc:
            context.abort(
                exc.code(),
                exc.details() or "callback relay failed",
            )
        except Exception as exc:
            context.abort(grpc.StatusCode.UNAVAILABLE, str(exc))
        return callbacks_pb2.CallbackResponse(request_id=request.request_id)


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--endpoint", required=True)
    p.add_argument("--id", default=f"coord-{os.getpid()}")
    p.add_argument("--result-mode", choices=("required", "legacy-observation-only"),
                   default="required")
    p.add_argument("--result-timeout", type=float, default=5.0)
    args = p.parse_args()

    if args.endpoint.startswith("unix:"):
        path = args.endpoint[5:]
        try: os.unlink(path)
        except FileNotFoundError: pass

    relay = Relay(args.result_mode, args.result_timeout)
    coord_svc = CoordSvc(args.id, relay)
    proxy_svc = ProxySvc(args.id, relay)

    srv = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    coordinator_pb2_grpc.add_HighBarCoordinatorServicer_to_server(coord_svc, srv)
    service_pb2_grpc.add_HighBarProxyServicer_to_server(proxy_svc, srv)
    srv.add_insecure_port(args.endpoint)
    srv.start()
    print(f"coordinator id={args.id} "
          f"(HighBarCoordinator + HighBarProxy) listening on {args.endpoint}",
          flush=True)
    try:
        srv.wait_for_termination()
    except KeyboardInterrupt:
        srv.stop(0)


if __name__ == "__main__":
    main()
