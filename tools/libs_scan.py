import io, re, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
p = r"C:\Program Files\Signalyst\HQPlayer 6 Desktop\HQPlayer6Desktop.exe"
data = open(p, "rb").read()
print("size", len(data))
names = [b"fftw", b"FFTW", b"soxr", b"sox", b"r8brain", b"pffft", b"kissfft", b"ipp", b"MKL", b"cufft", b"cuFFT", b"hip", b"HIP", b"vulkan", b"Vulkan", b"directml", b"DirectML", b"opencl", b"OpenCL", b"libsamplerate", b"samplerate", b"speex", b"zita", b"libav", b"ffmpeg", b"double128", b"Project", b"4321", b"8080", b"8088", b"hqp-control", b"control", b"upnp", b"UPnP", b"gapless", b"Matrix", b"matrix"]
for n in names:
    c = data.count(n)
    if c:
        print(n.decode(), c)
print("--- port-ish strings ---")
for m in re.finditer(rb"[ -~]{6,60}", data):
    s = m.group().decode("ascii", "replace")
    if re.search(r"(port|Port|listen|Listen)", s) and re.search(r"\d{2,5}", s):
        print(s)
