# Multi-stage build: Go is needed only here, never on the Kubernetes nodes.
FROM golang:1.27 AS builder
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /manager .

FROM gcr.io/distroless/static:nonroot
COPY --from=builder /manager /manager
USER nonroot:nonroot
ENTRYPOINT ["/manager"]
