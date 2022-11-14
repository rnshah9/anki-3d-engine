FROM --platform=linux/amd64 ubuntu:22.04 as builder

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake libx11-dev libxrandr-dev libx11-xcb-dev libxinerama-dev python-is-python3 clang

COPY . /repo
WORKDIR /repo/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release
RUN make
RUN clang++ /repo/mayhem/fuzz_sincos.cpp -fsanitize=fuzzer,address -o /fuzz -I /repo -I /repo/build/ -lAnKiMath -L /repo/build/AnKi/Math/ 

FROM ubuntu:22.04 as package
COPY --from=builder /repo/build/Bin/ShaderCompiler /
COPY --from=builder /fuzz /fuzz_sincos
