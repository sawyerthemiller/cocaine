import ctypes
import time

speech = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/SpeechSynthesis.framework/Versions/A/SpeechSynthesis")
speech.NewSpeechChannel.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
speech.SpeakText.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long]
speech.SpeechBusy.restype = ctypes.c_short
speech.DisposeSpeechChannel.argtypes = [ctypes.c_void_p]

channel = ctypes.c_void_p()
assert speech.NewSpeechChannel(None, ctypes.byref(channel)) == 0 and channel.value
try:
    for text in ("Testing speech completion.", "A second spoken message."):
        assert speech.SpeakText(channel, text, len(text)) == 0
        assert speech.SpeechBusy()
        deadline = time.time() + 15
        while speech.SpeechBusy() and time.time() < deadline:
            time.sleep(0.05)
        assert not speech.SpeechBusy(), "speech did not finish"
finally:
    assert speech.DisposeSpeechChannel(channel) == 0

print "speech audio compatibility ok"
