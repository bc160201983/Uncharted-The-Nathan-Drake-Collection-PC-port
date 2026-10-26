import sys,os,glob,re
from PIL import Image
D=sys.argv[1]; out=sys.argv[2]; tag=sys.argv[3] if len(sys.argv)>3 else 's'
os.makedirs(out,exist_ok=True)
for f in sorted(glob.glob(os.path.join(D,'dumps','output_*.raw'))):
  m=re.search(r'output_(\d+)_(\d+)x(\d+)',f); i,w,h=int(m[1]),int(m[2]),int(m[3])
  d=open(f,'rb').read()
  im=Image.frombytes('RGBA',(w,h),d); b,g,r,a=im.split()
  p=os.path.join(out,'%s_%03d.png'%(tag,i)); Image.merge('RGB',(r,g,b)).resize((w//2,h//2)).save(p); print(p)
