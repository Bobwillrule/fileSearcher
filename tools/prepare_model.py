"""Download pinned BGE assets and generate independent PyTorch reference outputs."""
import hashlib
import json
from pathlib import Path
import requests
import torch
from transformers import AutoTokenizer, AutoModel

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "models" / "bge-small-en-v1.5"
LOCK = json.loads((ROOT / "tools/model-assets.lock.json").read_text(encoding="utf-8"))
REVISION = LOCK["revision"]
PREFIX = "Represent this sentence for searching relevant passages: "
FILES = ["config.json", "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json", "vocab.txt", "model.safetensors", "onnx/model.onnx", "README.md"]

def main():
    MODEL.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for name in FILES:
        destination = MODEL / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            with requests.get(f"https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/{REVISION}/{name}", stream=True, timeout=120) as response:
                response.raise_for_status()
                temporary = destination.with_suffix(destination.suffix + ".download")
                with temporary.open("wb") as output:
                    for data in response.iter_content(1024 * 1024):
                        output.write(data)
                temporary.replace(destination)
        hashes[name] = hashlib.sha256(destination.read_bytes()).hexdigest()
        if hashes[name] != LOCK["sha256"][name]:
            raise RuntimeError(f"Asset checksum mismatch: {name}; remove this file and retry")
    lock = {"repository": "BAAI/bge-small-en-v1.5", "revision": REVISION, "sha256": hashes}
    (MODEL / "assets.lock.json").write_text(json.dumps(lock, indent=2), encoding="utf-8")
    tokenizer = AutoTokenizer.from_pretrained(MODEL, local_files_only=True)
    model = AutoModel.from_pretrained(MODEL, local_files_only=True, attn_implementation="eager").eval()
    torch.set_num_threads(4)
    corpus = json.loads((ROOT / "tests/fixtures/retrieval.json").read_text(encoding="utf-8"))
    cases = [{"id": d["id"], "text": d["text"], "query": False} for d in corpus["documents"]]
    cases += [{"id": "query" + str(i), "text": q["text"], "query": True, "relevant": q["relevant"], "kind": q["kind"]} for i, q in enumerate(corpus["queries"])]
    for i, value in enumerate(["", " ", "Café naïve résumé — 中文 東京 😀", "a\tline\nwith\u0000controls", "[CLS] hello [MASK] [SEP]", "word " * 600, "a" * 101, "İSTANBUL Straße e\u0301", "short"]):
        cases.append({"id": "edge" + str(i), "text": value, "query": False})
    for case in cases:
        text = (PREFIX if case["query"] else "") + case["text"]
        tokens = tokenizer(text, truncation=True, max_length=512, return_tensors="pt")
        with torch.inference_mode():
            embedding = torch.nn.functional.normalize(model(**tokens).last_hidden_state[:, 0], p=2, dim=1)[0]
        case["ids"] = tokens["input_ids"][0].tolist()
        case["embedding"] = embedding.tolist()
    (MODEL / "reference.json").write_text(json.dumps({"tolerance": 0.0001, "cases": cases}, ensure_ascii=False), encoding="utf-8")
    print(f"Prepared {len(cases)} reference cases; asset lock: {MODEL / 'assets.lock.json'}")

if __name__ == "__main__":
    main()
