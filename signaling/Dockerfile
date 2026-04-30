# syntax=docker/dockerfile:1.7

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

# Cache module downloads on go.mod/go.sum changes.
COPY go.mod ./
COPY go.sum* ./
RUN go mod download

COPY . .

# Static, stripped binary suitable for distroless.
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/signaling .

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/signaling /signaling
ENV SIGNALING_PORT=8080
EXPOSE 8080
USER nonroot:nonroot
ENTRYPOINT ["/signaling"]
