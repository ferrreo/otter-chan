# PlatformIO extra script: `pio run -t upload_models` flashes the ESP-SR model bundle (srmodels.bin
# from the Arduino core) into the `model` partition (offset from esp_sr_16.csv).
Import("env")
import os, glob

MODEL_OFFSET = "0xC10000"

def upload_models(source, target, env):
    libs = glob.glob(os.path.join(env.subst("$PROJECT_PACKAGES_DIR"), "framework-arduinoespressif32-libs*", "esp32s3", "esp_sr", "srmodels.bin"))
    if not libs:
        print("srmodels.bin not found in the Arduino core package")
        env.Exit(1)
    port = env.subst("$UPLOAD_PORT") or ""
    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", "esp32s3"] + (["--port", port] if port else []) + \
          ["--baud", env.subst("$UPLOAD_SPEED") or "921600", "write_flash", MODEL_OFFSET, libs[0]]
    print(" ".join(cmd))
    env.Execute(" ".join(cmd))

env.AddCustomTarget("upload_models", None, upload_models, title="Upload ESP-SR models", description="Flash srmodels.bin to the model partition")
