"""
KEYSTONE Neural Micro-Model Context Classifier (260->64->6 Feedforward).
"""

from enum import IntEnum
from typing import Tuple
import math


class SemanticClass(IntEnum):
    UNKNOWN = 0
    FINANCIAL = 1
    CORPORATE = 2
    GOVERNMENT = 3
    INFRASTRUCTURE = 4
    CONSUMER = 5


class NeuralClassifier:
    """
    Pythonic 6-Class Feedforward Micro-Model matching KEYSTONE's inlined C SAXPY kernel.
    """
    INPUT_DIM = 260
    HIDDEN_DIM = 64
    NUM_CLASSES = 6

    def __init__(self):
        # Deterministic synthetic model weights matching C backend
        self.w1 = [
            [(((j * 37 + i * 19 + 7) % 100) - 50) / 500.0 for i in range(self.HIDDEN_DIM)]
            for j in range(self.INPUT_DIM)
        ]
        self.b1 = [0.05] * self.HIDDEN_DIM
        self.w2 = [
            [(((j * 23 + i * 41 + 11) % 100) - 50) / 250.0 for i in range(self.NUM_CLASSES)]
            for j in range(self.HIDDEN_DIM)
        ]
        self.b2 = [0.0] * self.NUM_CLASSES

    def extract_features(self, text: str) -> list:
        x = [0.0] * self.INPUT_DIM
        if not text:
            return x

        char_counts = [0] * 256
        total = 0
        digits = 0
        uppercase = 0
        symbols = 0

        for char in text:
            code = ord(char) & 0xFF
            char_counts[code] += 1
            total += 1
            if char.isdigit():
                digits += 1
            elif char.isupper():
                uppercase += 1
            elif not char.isalnum() and not char.isspace():
                symbols += 1

        if total > 0:
            for i in range(256):
                x[i] = char_counts[i] / total
            x[256] = digits / total
            x[257] = uppercase / total
            x[258] = symbols / total
            x[259] = min(len(text) / 256.0, 1.0)
        return x

    def classify(self, text: str) -> Tuple[SemanticClass, str, float]:
        x = self.extract_features(text)

        # Hidden layer
        hidden = list(self.b1)
        for j in range(self.INPUT_DIM):
            xj = x[j]
            if xj == 0.0:
                continue
            wrow = self.w1[j]
            for i in range(self.HIDDEN_DIM):
                hidden[i] += xj * wrow[i]

        for i in range(self.HIDDEN_DIM):
            hidden[i] = max(hidden[i], 0.0)  # ReLU

        # Output layer
        scores = list(self.b2)
        for j in range(self.HIDDEN_DIM):
            hj = hidden[j]
            if hj == 0.0:
                continue
            wrow = self.w2[j]
            for i in range(self.NUM_CLASSES):
                scores[i] += hj * wrow[i]

        max_score = max(scores)
        best_cls = scores.index(max_score)

        exp_scores = [math.exp(s - max_score) for s in scores]
        sum_exp = sum(exp_scores)
        conf = exp_scores[best_cls] / sum_exp if sum_exp > 0 else 0.0

        cls_enum = SemanticClass(best_cls)
        return cls_enum, cls_enum.name, conf
