//go:build !linux

package v4l2

// Open on non-Linux platforms always returns ErrUnsupported.
//
// The binary is shipped only in a Linux container in production; the
// fallback exists so `go build` + `go test` work on macOS/Windows
// dev machines without conditional compilation surprises.
func Open(_ string, _ VideoFormat) (Writer, error) {
	return nil, ErrUnsupported
}
