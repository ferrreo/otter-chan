package vision

import (
	"bytes"
	"image"
	"image/color"
	"image/jpeg"
	"testing"
)

func TestNoFaceOnBlank(t *testing.T) {
	img := image.NewGray(image.Rect(0, 0, 160, 120))
	for i := range img.Pix {
		img.Pix[i] = uint8(i % 7 * 30)
	}
	var buf bytes.Buffer
	jpeg.Encode(&buf, img, nil)
	if r := FindFace(buf.Bytes()); r.Found {
		t.Fatalf("unexpected face %+v", r)
	}
	if r := FindFace([]byte("not a jpeg")); r.Found {
		t.Fatal("garbage decoded as face")
	}
	_ = color.Gray{}
}
