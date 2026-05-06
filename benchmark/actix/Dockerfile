FROM rust:latest AS builder
WORKDIR /app
COPY Cargo.toml ./
COPY src/ ./src/
RUN cargo build --release

FROM debian:bookworm-slim
COPY --from=builder /app/target/release/benchmark-actix /server
EXPOSE 8082
CMD ["/server"]
