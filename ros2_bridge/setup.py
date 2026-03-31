from setuptools import setup, find_packages

setup(
    name="ros2_bridge",
    version="0.1.0",
    packages=find_packages(),
    install_requires=[
        "websockets>=11.0",
    ],
    entry_points={
        "console_scripts": [
            "bridge_node = ros2_bridge.bridge_node:main",
        ],
    },
)
