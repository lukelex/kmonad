FROM haskell:9.6.7-bullseye AS development

WORKDIR /workspace

RUN git config --system --add safe.directory /workspace

# Keep Hackage metadata in the image layer. Source and build artifacts are
# mounted by Compose, so local commands never alter the host toolchain.
RUN cabal update

CMD ["cabal", "test", "all", "--enable-tests", "--test-show-details=direct"]
