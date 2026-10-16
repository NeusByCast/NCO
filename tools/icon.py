"""Build NCO's original vector-style application icon; no external assets."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
out = Path(__file__).resolve().parents[1] / 'assets'
im = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((24, 24, 1000, 1000), 232, fill='#241a13')
d.rounded_rectangle((104, 104, 920, 920), 176, outline='#65462e', width=8)
# A continuous N with a small recording light.
d.line([(280, 708), (280, 316), (644, 708), (644, 316)], fill='#dfb48a', width=76, joint='curve')
d.ellipse((716, 252, 820, 356), fill='#c68c59')
im.save(out / 'nco.ico', sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
im.resize((256, 256), Image.Resampling.LANCZOS).save(out / 'nco.png')

def font(size):
    for path in ('C:/Windows/Fonts/segoeuib.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)

# Original installer artwork, rendered at 4x before downsampling.
wizard = Image.new('RGB', (656, 1256), '#241a13')
logo = im.resize((480, 480), Image.Resampling.LANCZOS)
wizard.paste(logo, (88, 160), logo)
draw = ImageDraw.Draw(wizard)
draw.text((328, 750), 'NCO', font=font(124), fill='#eee4da', anchor='mt')
draw.text((328, 928), 'CAPTURE, REFINED.', font=font(29), fill='#c68c59', anchor='mt')
wizard.resize((164, 314), Image.Resampling.LANCZOS).save(out / 'wizard.bmp')
small = Image.new('RGB', (55, 55), '#30251d')
logo = im.resize((51, 51), Image.Resampling.LANCZOS)
small.paste(logo, (2, 2), logo)
small.save(out / 'wizard-small.bmp')
