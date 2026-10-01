#!/usr/bin/env python3
import hashlib,json,re,struct,sys
from pathlib import Path
repo=Path(__file__).resolve().parents[2]
packet=repo/'contracts/barc-stock-queue-v1'
contract=json.loads((packet/'contract.json').read_text())
vectors=json.loads((packet/'vectors.json').read_text())
assert contract['schema']=='barc.stock-queue-contract/1'
assert contract['profiles']['legacy']=={'name':'barc-live-tactical-v1','revision':1,'acceptedEvidenceSchemeNumbers':[0,1],'defaultZeroMeaning':'full-native-tuple-v1'}
assert contract['profiles']['stock']['name']=='barc-live-tactical-stock-v1' and contract['profiles']['stock']['revision']==2
assert contract['profiles']['stock']['requiredEvidenceSchemeNumber']==2
assert contract['factoryProductionPolicies']['replace'].startswith('unsupported including empty queue')
assert contract['unavailableFields']==['timeout']
assert contract['bridge']['limits']=={
    'maxInputBytes':8192,'maxResponseBytes':8192,'maxEntries':64,
    'overflowProbeEntries':65,'maxParamsPerEntry':16,
    'maxTotalParams':256,'maxLineBytes':512}
assert contract['bridge']['float32'].startswith('exactly eight lowercase IEEE-754 binary32 hex digits')
by={v['name']:v for v in vectors['vectors']}
for value in by.values():
    raw=value['ascii'].encode('ascii')
    assert b'\0' not in raw and len(raw)<=8192 and raw.endswith(b'end\n')
    assert value['byteLength']==len(raw) and value['sha256']==hashlib.sha256(raw).hexdigest()
    lines=value['ascii'].splitlines()
    assert re.fullmatch(r'length=[0-9]{8}',lines[1]) and int(lines[1][7:])==len(raw)
assert by['production-request']['sha256']==vectors['requestSha256']
for name in ['empty-production','one-production-with-negative-zero','wrong-team-unavailable']:
    assert f'request-sha256={vectors["requestSha256"]}' in by[name]['ascii']
assert 'row=production|-710|32|41|3f800000,80000000\n' in by['one-production-with-negative-zero']['ascii']
revision=vectors['revisionVector'];context=revision['context']
preimage=bytearray(b'barc-stock-queue-revision/1\0')
def field(tag,value):
    preimage.append(tag);preimage.extend(struct.pack('>I',len(value)));preimage.extend(value)
field(1,struct.pack('>I',context['evidenceScheme']))
field(2,context['profile'].encode('utf-8'))
field(3,struct.pack('>I',context['revision']))
field(4,bytes.fromhex(context['catalogueIdHex']))
field(5,struct.pack('>Q',int(context['catalogueRevision'])))
field(6,context['engineVersion'].encode('utf-8'))
field(7,context['gameName'].encode('utf-8'))
field(8,context['gameVersion'].encode('utf-8'))
field(9,bytes.fromhex(context['gameContentSha256Hex']))
field(10,struct.pack('>I',context['actorId']))
field(11,struct.pack('>Q',int(context['actorLifetime'])))
field(12,context['domain'].encode('ascii'))
field(13,struct.pack('>I',len(context['rows'])))
for row in context['rows']: field(14,row.encode('ascii'))
digest=hashlib.sha256(preimage).digest()
assert len(preimage)==revision['preimageByteLength'] and preimage.hex()==revision['preimageHex']
assert digest.hex()==revision['sha256'] and str(int.from_bytes(digest[:8],'big') or 1)==revision['projectedUint64']
proto=(repo/'proto/highbar/live_control.proto').read_text()
assert 'NATIVE_QUEUE_EVIDENCE_SCHEME_FULL_NATIVE_TUPLE_V1 = 1;' in proto
assert 'NATIVE_QUEUE_EVIDENCE_SCHEME_STOCK_LUA_SUPPORTED_FIELDS_V1 = 2;' in proto
assert re.search(r'NativeQueueEvidenceScheme evidence_scheme = 6;',proto)
assert 'timeout is unavailable and is never fabricated' in proto
print('stock queue contract vectors: PASS')
