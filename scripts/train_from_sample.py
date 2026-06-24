"""
Train the KEYSTONE micro-model classifier on a real research data sample.
- Random file selection across the full corpus (not sequential)
- 6 output classes: GENERIC, FINANCIAL, CORPORATE, GOVERNMENT, HEALTHCARE, TECHNOLOGY
- Records with no job data are labelled CLASS_UNKNOWN (excluded from training —
  the hard rule in C handles those at runtime without needing learned weights)
- Class-weighted cross-entropy loss to prevent majority-class collapse
- 260-dim input: 256 trigram features + 4 char-ratio features
- Confidence gating: model returns UNKNOWN if max softmax < 0.50
Exports trained weights to include/dsmil_model_weights.h
"""

import gzip, json, os, random
import numpy as np

VAULT               = "/vault/archives/CONSOLIDATED/RESEARCH_DATA/gz"
SAMPLE_PER_FILE     = 1500      # pull more per file so minority classes have headroom
NUM_FILES           = 16        # wider random draw across corpus
INPUT_DIM           = 260       # 256 trigram + 4 char-ratio
HIDDEN_DIM          = 64
NUM_CLASSES         = 6
EPOCHS              = 500
LR                  = 0.008
BATCH_SIZE          = 64
CONFIDENCE_THRESHOLD = 0.30     # must match DSMIL_CONFIDENCE_THRESHOLD in dsmil_micro_model.h
GENERIC_CAP_RATIO   = 2.0       # cap GENERIC at 2x the second-largest class (no floor games)
FOCAL_GAMMA         = 2.0       # focal loss: down-weights easy GENERIC examples automatically

# --- Class definitions (must match dsmil_micro_model.h) ---
# CLASS_GENERIC=0  CLASS_FINANCIAL=1  CLASS_CORPORATE=2
# CLASS_GOVERNMENT=3  CLASS_HEALTHCARE=4  CLASS_TECHNOLOGY=5
# CLASS_UNKNOWN=99  (not trained; handled by hard rule in C)

FINANCIAL_ROLES    = {"finance","accounting","operations","legal","purchasing","insurance"}
FINANCIAL_INDUSTRY = {"financial services","banking","accounting","insurance",
                      "venture capital","investment management","capital markets",
                      "investment banking","financial services"}
CORPORATE_ROLES    = {"cxo","director","vp","manager","owner","president","partner","board"}
CORPORATE_LEVELS   = {"cxo","vp","director","c-suite","partner","owner"}
GOVERNMENT_ROLES   = {"government","military","public administration","defence","nonprofit"}
GOVERNMENT_INDUSTRY= {"government administration","military","law enforcement","public policy",
                      "political organization","think tanks","non-profit organization management"}
HEALTHCARE_INDUSTRY= {"hospital & health care","medical devices","pharmaceuticals","biotechnology",
                      "health wellness and fitness","mental health care","veterinary","medical practice"}
TECHNOLOGY_INDUSTRY= {"information technology and services","computer software","internet",
                      "computer networking","semiconductors","computer hardware","cybersecurity",
                      "telecommunications","wireless","computer & network security"}
TECHNOLOGY_ROLES   = {"engineering","it","data science","artificial intelligence","devops"}

def classify_record(r):
    role     = (r.get("job_title_role") or "").lower()
    levels   = " ".join(r.get("job_title_levels") or []).lower()
    industry = (r.get("industry") or "").lower()
    co_ind   = (r.get("job_company_industry") or "").lower()
    title    = (r.get("job_title") or "").lower()

    # Records with no job context are excluded from training
    # (handled at runtime by the hard UNKNOWN rule in C)
    if not role and not industry and not title:
        return None

    if industry in GOVERNMENT_INDUSTRY or co_ind in GOVERNMENT_INDUSTRY or "government" in industry:
        return 3  # GOVERNMENT
    if industry in HEALTHCARE_INDUSTRY or co_ind in HEALTHCARE_INDUSTRY:
        return 4  # HEALTHCARE
    if industry in TECHNOLOGY_INDUSTRY or co_ind in TECHNOLOGY_INDUSTRY or role in TECHNOLOGY_ROLES:
        return 5  # TECHNOLOGY
    if role in FINANCIAL_ROLES or industry in FINANCIAL_INDUSTRY or co_ind in FINANCIAL_INDUSTRY:
        return 1  # FINANCIAL
    if role in CORPORATE_ROLES or any(l in levels for l in CORPORATE_LEVELS):
        return 2  # CORPORATE
    return 0  # GENERIC

def hash_trigram(a, b, c):
    h = 2166136261
    for x in (a, b, c):
        h = ((h ^ ord(x)) * 16777619) & 0xFFFFFFFF
    return h % 256

def extract_features(text):
    t = str(text).lower()
    vec = np.zeros(INPUT_DIM, dtype=np.float32)

    # Trigram features [0..255]
    for i in range(len(t) - 2):
        vec[hash_trigram(t[i], t[i+1], t[i+2])] += 1.0
    s = vec[:256].sum()
    if s > 0: vec[:256] /= s

    # Char-ratio features [256..259]
    if len(t) > 0:
        n = float(len(t))
        digits   = sum(1 for c in t if c.isdigit())
        uppers   = sum(1 for c in t if c.isupper())
        specials = sum(1 for c in t if not c.isalnum() and not c.isspace())
        spaces   = sum(1 for c in t if c.isspace())
        vec[256] = digits   / n
        vec[257] = uppers   / n
        vec[258] = specials / n
        vec[259] = spaces   / n
    return vec

def record_to_features(r):
    context = " ".join(filter(None, [
        r.get("job_title"), r.get("industry"), r.get("job_company_name"),
        r.get("job_company_industry"), r.get("job_title_role"),
        r.get("location_name"), r.get("summary"),
        " ".join(r.get("job_title_levels") or []),
    ]))
    return extract_features(context)

# --- Random file selection ---
all_gz   = sorted([f for f in os.listdir(VAULT) if f.endswith(".gz")])
selected = random.sample(all_gz, min(NUM_FILES, len(all_gz)))
print(f"Sampling {SAMPLE_PER_FILE} records from {NUM_FILES} randomly selected files...\n")

X, y = [], []
skipped_no_job = 0
for fname in selected:
    path = os.path.join(VAULT, fname)
    print(f"  {fname}...", end=" ", flush=True)
    sampled = 0
    try:
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        random.shuffle(lines)
        for line in lines:
            if sampled >= SAMPLE_PER_FILE: break
            line = line.strip()
            if not line: continue
            try:
                r   = json.loads(line)
                lbl = classify_record(r)
                if lbl is None:
                    skipped_no_job += 1
                    continue
                X.append(record_to_features(r))
                y.append(lbl)
                sampled += 1
            except Exception:
                pass
    except Exception as e:
        print(f"[skip: {e}]", end=" ")
    print(f"{sampled} records")

X = np.array(X, dtype=np.float32)
y = np.array(y, dtype=np.int32)
counts = np.bincount(y, minlength=NUM_CLASSES)
class_names = ["GENERIC","FINANCIAL","CORPORATE","GOVERNMENT","HEALTHCARE","TECHNOLOGY"]
print(f"\nTotal: {len(X)} samples  (excluded {skipped_no_job} no-job records → UNKNOWN at runtime)")
for i, name in enumerate(class_names):
    print(f"  {name:12s}: {counts[i]}")

# --- Undersample GENERIC to prevent majority collapse ---
# Find the second-largest class count and cap GENERIC at 2x that
sorted_counts = np.sort(counts)[::-1]
second_largest = sorted_counts[1] if len(sorted_counts) > 1 else counts[0]
generic_cap = int(second_largest * GENERIC_CAP_RATIO)

if counts[0] > generic_cap:
    generic_idx   = np.where(y == 0)[0]
    keep_generic  = np.random.choice(generic_idx, generic_cap, replace=False)
    other_idx     = np.where(y != 0)[0]
    all_idx       = np.concatenate([keep_generic, other_idx])
    np.random.shuffle(all_idx)
    X, y = X[all_idx], y[all_idx]
    counts = np.bincount(y, minlength=NUM_CLASSES)
    print(f"\nAfter GENERIC undersampling (cap={generic_cap}):")
    for i, name in enumerate(class_names):
        print(f"  {name:12s}: {counts[i]}")

# Standard inverse-frequency weights on balanced set (no floor)
eps = 1.0
class_weights = (len(X) + eps) / (NUM_CLASSES * (counts.astype(np.float32) + eps))
class_weights = class_weights / class_weights.sum() * NUM_CLASSES
print("\nClass weights after balancing:")
for i, name in enumerate(class_names):
    print(f"  {name:12s}: {class_weights[i]:.2f}")
sample_weights = class_weights[y]

# --- Train ---
np.random.seed(42)
W1 = np.random.randn(INPUT_DIM, HIDDEN_DIM).astype(np.float32) * 0.1
B1 = np.zeros(HIDDEN_DIM, dtype=np.float32)
W2 = np.random.randn(HIDDEN_DIM, NUM_CLASSES).astype(np.float32) * 0.1
B2 = np.zeros(NUM_CLASSES, dtype=np.float32)

def relu(x): return np.maximum(0, x)
def softmax(x):
    e = np.exp(x - x.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)

n = len(X)
print(f"\nTraining {EPOCHS} epochs...\n")
for epoch in range(EPOCHS):
    idx = np.random.permutation(n)
    Xs, ys, ws = X[idx], y[idx], sample_weights[idx]
    total_loss  = 0

    for i in range(0, n, BATCH_SIZE):
        Xb = Xs[i:i+BATCH_SIZE]
        yb = ys[i:i+BATCH_SIZE]
        wb = ws[i:i+BATCH_SIZE]
        m  = len(yb)

        h      = relu(Xb @ W1 + B1)
        logits = h @ W2 + B2
        probs  = softmax(logits)

        # Focal loss: (1 - p_t)^gamma * -log(p_t)
        # Down-weights easy/confident examples so GENERIC majority can't dominate
        p_t      = probs[np.arange(m), yb]
        focal_w  = (1.0 - p_t) ** FOCAL_GAMMA
        loss_per = -np.log(p_t + 1e-9) * focal_w
        loss     = (loss_per * wb).mean()
        total_loss += loss

        dlogits = probs.copy()
        dlogits[np.arange(m), yb] -= 1
        dlogits *= (wb[:, None] / m)

        dW2 = h.T @ dlogits;  dB2 = dlogits.sum(0)
        dh  = dlogits @ W2.T; dh[h <= 0] = 0
        dW1 = Xb.T @ dh;      dB1 = dh.sum(0)

        W2 -= LR * dW2; B2 -= LR * dB2
        W1 -= LR * dW1; B1 -= LR * dB1

    if epoch % 100 == 0:
        h     = relu(X @ W1 + B1)
        probs = softmax(h @ W2 + B2)
        preds = probs.argmax(1)
        acc   = (preds == y).mean()
        recalls = [(preds[y==c] == c).mean() if counts[c] > 0 else 0.0 for c in range(NUM_CLASSES)]
        gated_unknown = (probs.max(1) < CONFIDENCE_THRESHOLD).mean()
        row = " | ".join(f"{class_names[c][0]}={recalls[c]:.2f}" for c in range(NUM_CLASSES))
        print(f"  Epoch {epoch:3d} | loss={total_loss/n:.4f} | acc={acc:.3f} | {row} | unk={gated_unknown:.2f}")

# --- Final report ---
h     = relu(X @ W1 + B1)
probs = softmax(h @ W2 + B2)
preds = probs.argmax(1)
acc   = (preds == y).mean()
recalls = [(preds[y==c] == c).mean() if counts[c] > 0 else 0.0 for c in range(NUM_CLASSES)]
gated_unknown = (probs.max(1) < CONFIDENCE_THRESHOLD).mean()

print(f"\n{'='*60}")
print(f"Final accuracy:  {acc:.3f}")
print(f"UNKNOWN gated:   {gated_unknown:.1%} of training set below confidence threshold")
for i, name in enumerate(class_names):
    print(f"  {name:12s} recall: {recalls[i]:.3f}  (n={counts[i]})")

# --- Export weights ---
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../include/dsmil_model_weights.h")
recall_str = " ".join(f"{class_names[c][0]}={recalls[c]:.2f}" for c in range(NUM_CLASSES))
with open(out, "w") as f:
    f.write("/* AUTO-GENERATED by scripts/train_from_sample.py — DO NOT EDIT */\n")
    f.write(f"/* Samples={len(X)} acc={acc:.3f} | {recall_str} */\n")
    f.write(f"/* INPUT_DIM={INPUT_DIM} HIDDEN_DIM={HIDDEN_DIM} NUM_CLASSES={NUM_CLASSES} */\n")
    f.write("#ifndef DSMIL_MODEL_WEIGHTS_H\n#define DSMIL_MODEL_WEIGHTS_H\n\n")

    f.write(f"static const float MODEL_W1[{INPUT_DIM}][{HIDDEN_DIM}] = {{\n")
    for row in W1:
        f.write("    {" + ", ".join(f"{x:.6f}f" for x in row) + "},\n")
    f.write("};\n\n")

    f.write(f"static const float MODEL_B1[{HIDDEN_DIM}] = {{")
    f.write(", ".join(f"{x:.6f}f" for x in B1))
    f.write("};\n\n")

    f.write(f"static const float MODEL_W2[{HIDDEN_DIM}][{NUM_CLASSES}] = {{\n")
    for row in W2:
        f.write("    {" + ", ".join(f"{x:.6f}f" for x in row) + "},\n")
    f.write("};\n\n")

    f.write(f"static const float MODEL_B2[{NUM_CLASSES}] = {{")
    f.write(", ".join(f"{x:.6f}f" for x in B2))
    f.write("};\n\n#endif\n")

print(f"\nWeights written → {out}")
