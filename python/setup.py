from setuptools import setup, find_packages

setup(
    name="keystone-sdk",
    version="1.0.0",
    description="High-Performance Target-Silicon-Tuned Search & Telemetry Engine",
    author="John Reese",
    packages=find_packages(),
    install_requires=[
        "numpy>=1.20.0",
    ],
    python_requires=">=3.8",
)
