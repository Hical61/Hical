FROM golang:1.22-alpine AS builder
WORKDIR /app
COPY go.mod ./
COPY main.go ./
RUN go mod tidy && CGO_ENABLED=0 go build -ldflags="-s -w" -o server .

FROM alpine:3.19
COPY --from=builder /app/server /server
EXPOSE 8081
CMD ["/server"]
