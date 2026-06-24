import numpy as np

# Synthetic training: generate random weights for the C inference engine
W1 = np.random.randn(256, 64) * 0.1
B1 = np.random.randn(64) * 0.1
W2 = np.random.randn(64, 3) * 0.1
B2 = np.random.randn(3) * 0.1

with open("../include/dsmil_model_weights.h", "w") as f:
    f.write("#ifndef DSMIL_MODEL_WEIGHTS_H\n#define DSMIL_MODEL_WEIGHTS_H\n\n")
    
    f.write("static const float MODEL_W1[256][64] = {\n")
    for row in W1:
        f.write("    {" + ", ".join(f"{x:.6f}f" for x in row) + "},\n")
    f.write("};\n\n")
    
    f.write("static const float MODEL_B1[64] = {")
    f.write(", ".join(f"{x:.6f}f" for x in B1))
    f.write("};\n\n")
    
    f.write("static const float MODEL_W2[64][3] = {\n")
    for row in W2:
        f.write("    {" + ", ".join(f"{x:.6f}f" for x in row) + "},\n")
    f.write("};\n\n")
    
    f.write("static const float MODEL_B2[3] = {")
    f.write(", ".join(f"{x:.6f}f" for x in B2))
    f.write("};\n\n")
    
    f.write("#endif\n")
print("Generated synthetic weights in dsmil_model_weights.h")
