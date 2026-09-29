# Writes the harness's test card: an original pattern of colour bars, a grey
# ramp, a circle and fine vertical detail. testcard.png to look at,
# testcard.rgba (1280x720 RGBA, bottom-up like a GL host texture) to feed in.
from PIL import Image, ImageDraw

W, H = 1280, 720
im = Image.new('RGB', (W, H), (18, 18, 24))
d = ImageDraw.Draw(im)
cols = [(235, 235, 235), (235, 235, 16), (16, 235, 235), (16, 235, 16), (235, 16, 235), (235, 16, 16), (16, 16, 235)]
for i, c in enumerate(cols):
    d.rectangle([i * W // 7, 0, (i + 1) * W // 7, H // 3], fill=c)
for x in range(W):
    g = int(255 * x / (W - 1))
    d.line([x, H // 3, x, H // 3 + 60], fill=(g, g, g))
d.ellipse([W // 2 - 200, H // 2 - 120, W // 2 + 200, H // 2 + 280], fill=(220, 120, 40), outline=(255, 255, 255), width=6)
for k in range(0, 300, 6):
    d.line([60 + k, 500, 60 + k, 690], fill=(255, 255, 255) if (k // 6) % 2 == 0 else (0, 0, 0), width=3)
d.rectangle([W - 360, 480, W - 60, 690], fill=(40, 90, 200))
d.text((W - 330, 560), "SKILLET TEST", fill=(255, 255, 255))
im.save('testcard.png')
with open('testcard.rgba', 'wb') as f:
    f.write(im.transpose(Image.FLIP_TOP_BOTTOM).convert('RGBA').tobytes())
