FROM golang:1.24-alpine AS builder
WORKDIR /app
COPY go.mod ./
COPY main.go ./
RUN go mod tidy && CGO_ENABLED=0 go build -ldflags="-s -w" -o server .

FROM alpine:3.20
COPY --from=builder /app/server /server
ENV GODEBUG=asynctimerchan=1
EXPOSE 8081
CMD ["/server"]
