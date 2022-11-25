FROM --platform=linux/amd64 ubuntu:22.04 as builder

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential git llvm-11-dev clang

RUN ln -s /usr/bin/llvm-config-11 /usr/bin/llvm-config

COPY . /repo
WORKDIR /repo
RUN make -j8
RUN cp /repo/src/* /repo
WORKDIR /repo/src
RUN clang++ /repo/mayhem/fuzz_OpenFile.cpp -fsanitize=fuzzer,address -I /repo/include/ -I /repo/ -L /repo/ -lxl -o /fuzz

FROM ubuntu:22.04 as package
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y llvm-11-dev

COPY --from=builder /repo /repo
COPY --from=builder /fuzz /fuzz_OpenFile
ENV LD_LIBRARY_PATH=/repo

WORKDIR /repo/src
