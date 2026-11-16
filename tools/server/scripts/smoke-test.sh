#!/usr/bin/env bash
# Smoke test of a running ixian-server (default http://127.0.0.1:9931): one request per feature the loaded package has.
#
#   tools/server/scripts/smoke-test.sh                         # local server
#   tools/server/scripts/smoke-test.sh http://iq9:8080 sk-key   # remote, with an API key
set -u
BASE="${1:-http://127.0.0.1:9931}"
KEY="${2:-}"
AUTH=()
[ -n "$KEY" ] && AUTH=(-H "Authorization: Bearer $KEY")
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/samples"
fail=0
check() { # name, command...
    local name="$1"; shift
    if out=$("$@" 2>&1); then echo "ok    $name"; else echo "FAIL  $name"; echo "$out" | head -5; fail=1; fi
}
jq_ok() { python3 -c 'import json,sys; d=json.load(sys.stdin); assert '"$1"', d' ; }

echo "== $BASE"
check "health"  bash -c "curl -fsS $BASE/health | jq_ok \"d['status']=='ok'\""
props=$(curl -fsS "${AUTH[@]}" "$BASE/props") || { echo "FAIL  props"; exit 1; }
echo "$props" | python3 -c 'import json,sys; d=json.load(sys.stdin); ix=d["ixian"]; print("      model", ix["model_id"], ix["target"], ix["nsp_count"], "NSP; modalities", d["modalities"], "; speech", ix["capabilities"]["speech_out"], "; decision", ix["capabilities"]["decision"])'
decision=$(echo "$props" | python3 -c 'import json,sys; print(str(json.load(sys.stdin)["ixian"]["capabilities"]["decision"]).lower())')
vision=$(echo "$props" | python3 -c 'import json,sys; print(str(json.load(sys.stdin)["modalities"]["vision"]).lower())')
audio=$(echo "$props" | python3 -c 'import json,sys; print(str(json.load(sys.stdin)["modalities"]["audio"]).lower())')
speech=$(echo "$props" | python3 -c 'import json,sys; print(str(json.load(sys.stdin)["ixian"]["capabilities"]["speech_out"]).lower())')

if [ "$decision" = "true" ]; then
    check "systemone" bash -c "curl -fsS ${AUTH[*]} $BASE/v1/systemone -H 'Content-Type: application/json' -d '{\"state\": \"I was charged twice and my parcel never arrived. Please refund one charge.\", \"questions\": {\"refund\": {\"type\": \"noul\", \"instructions\": \"Is the customer asking for a refund?\"}, \"team\": {\"type\": \"choice\", \"instructions\": \"Which team should handle this?\", \"criteria\": {\"billing\": \"Charges, refunds, invoices\", \"shipping\": \"Delivery and parcels\"}}}}' | jq_ok \"'refund' in d['answers'] and 0<=d['answers']['refund']['noul']<=1\""
    if [ "$vision" = "true" ]; then
        check "systemone image" bash -c "curl -fsS ${AUTH[*]} $BASE/v1/systemone -H 'Content-Type: application/json' -d \"{\\\"state\\\": null, \\\"images\\\": [\\\"data:image/png;base64,\$(base64 -w0 $S/cats.png)\\\"], \\\"questions\\\": {\\\"animal\\\": {\\\"type\\\": \\\"noul\\\", \\\"instructions\\\": \\\"Does the image show an animal?\\\"}}}\" | jq_ok \"'animal' in d['answers']\""
    fi
    [ $fail = 0 ] && echo "== all good" ; exit $fail
fi

check "models"       bash -c "curl -fsS ${AUTH[*]} $BASE/v1/models | jq_ok \"len(d['data'])==1\""
check "tokenize"     bash -c "curl -fsS ${AUTH[*]} $BASE/tokenize -d '{\"content\": \"hello world\"}' | jq_ok \"len(d['tokens'])>=2\""
check "chat"         bash -c "curl -fsS ${AUTH[*]} $BASE/v1/chat/completions -H 'Content-Type: application/json' -d '{\"max_tokens\": 32, \"messages\": [{\"role\": \"user\", \"content\": \"What is the capital of France?\"}]}' | jq_ok \"d['choices'][0]['message']['content'] and d['usage']['completion_tokens']>0\""
check "chat stream"  bash -c "curl -fsSN ${AUTH[*]} $BASE/v1/chat/completions -H 'Content-Type: application/json' -d '{\"max_tokens\": 16, \"stream\": true, \"messages\": [{\"role\": \"user\", \"content\": \"Count to five.\"}]}' | grep -c '^data: ' | awk '{exit !(\$1>2)}'"
check "completion"   bash -c "curl -fsS ${AUTH[*]} $BASE/completion -d '{\"prompt\": \"Once upon a time\", \"n_predict\": 16}' | jq_ok \"d['content'] and d['timings']['predicted_n']>0\""
check "anthropic"    bash -c "curl -fsS ${AUTH[*]} $BASE/v1/messages -H 'Content-Type: application/json' -d '{\"model\": \"x\", \"max_tokens\": 16, \"messages\": [{\"role\": \"user\", \"content\": \"Hello\"}]}' | jq_ok \"d['type']=='message'\""
if [ "$vision" = "true" ]; then
    check "image"    bash -c "curl -fsS ${AUTH[*]} $BASE/v1/chat/completions -H 'Content-Type: application/json' -d \"{\\\"max_tokens\\\": 32, \\\"messages\\\": [{\\\"role\\\": \\\"user\\\", \\\"content\\\": [{\\\"type\\\": \\\"image_url\\\", \\\"image_url\\\": {\\\"url\\\": \\\"data:image/png;base64,\$(base64 -w0 $S/cats.png)\\\"}}, {\\\"type\\\": \\\"text\\\", \\\"text\\\": \\\"How many cats are in the image?\\\"}]}]}\" | jq_ok \"d['usage']['prompt_tokens']>256\""
fi
if [ "$audio" = "true" ]; then
    check "audio in"      bash -c "curl -fsS ${AUTH[*]} $BASE/v1/chat/completions -H 'Content-Type: application/json' -d \"{\\\"max_tokens\\\": 32, \\\"messages\\\": [{\\\"role\\\": \\\"user\\\", \\\"content\\\": [{\\\"type\\\": \\\"input_audio\\\", \\\"input_audio\\\": {\\\"data\\\": \\\"\$(base64 -w0 $S/question.wav)\\\", \\\"format\\\": \\\"wav\\\"}}]}]}\" | jq_ok \"d['choices'][0]['message']['content']\""
    check "transcription" bash -c "curl -fsS ${AUTH[*]} $BASE/v1/audio/transcriptions -F file=@$S/jfk.wav -F language=en | jq_ok \"len(d['text'])>0\""
fi
if [ "$speech" = "true" ]; then
    check "speech"   bash -c "curl -fsS ${AUTH[*]} $BASE/v1/audio/speech -H 'Content-Type: application/json' -d '{\"input\": \"The quick brown fox jumps over the lazy dog.\", \"voice\": \"female\"}' -o /tmp/ixian-smoke.wav && head -c 4 /tmp/ixian-smoke.wav | grep -q RIFF"
fi
[ $fail = 0 ] && echo "== all good"
exit $fail
