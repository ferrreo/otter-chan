// Package vision finds the largest face in a JPEG using pigo (pure Go).
package vision

import (
	"bytes"
	_ "embed"
	"image"
	"image/jpeg"

	pigo "github.com/esimov/pigo/core"
)

//go:embed facefinder
var cascade []byte

type Result struct {
	Found bool    `json:"found"`
	X     float64 `json:"x,omitempty"`
	Y     float64 `json:"y,omitempty"`
	W     float64 `json:"w,omitempty"`
}

var classifier *pigo.Pigo

func init() {
	c, err := pigo.NewPigo().Unpack(cascade)
	if err != nil {
		panic("pigo cascade: " + err.Error())
	}
	classifier = c
}

func FindFace(data []byte) Result {
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		return Result{}
	}
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	gray := make([]uint8, w*h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			r, g, bl, _ := img.At(b.Min.X+x, b.Min.Y+y).RGBA()
			gray[y*w+x] = uint8((299*r + 587*g + 114*bl) / 1000 >> 8)
		}
	}
	params := pigo.CascadeParams{
		MinSize: 20, MaxSize: h, ShiftFactor: 0.1, ScaleFactor: 1.1,
		ImageParams: pigo.ImageParams{Pixels: gray, Rows: h, Cols: w, Dim: w},
	}
	dets := classifier.ClusterDetections(classifier.RunCascade(params, 0.0), 0.2)
	var best pigo.Detection
	for _, d := range dets {
		if d.Q >= 5.0 && d.Scale > best.Scale {
			best = d
		}
	}
	if best.Scale == 0 {
		return Result{}
	}
	return Result{Found: true, X: float64(best.Col) / float64(w), Y: float64(best.Row) / float64(h), W: float64(best.Scale) / float64(w)}
}

var _ = image.Point{}
