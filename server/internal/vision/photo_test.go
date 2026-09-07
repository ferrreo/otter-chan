package vision

import (
	"os"
	"testing"
)

// Set OTTER_VISION_TEST_JPEG to a photo with a face to check the detector.
func TestFindFaceOnPhoto(t *testing.T) {
	p := os.Getenv("OTTER_VISION_TEST_JPEG")
	if p == "" {
		t.Skip("set OTTER_VISION_TEST_JPEG")
	}
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	r := FindFace(b)
	t.Logf("result: %+v", r)
	if !r.Found {
		t.Fatal("no face found")
	}
}
