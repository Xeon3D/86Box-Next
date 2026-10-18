"""pullres.py VMDIR DEST -- copy C:/CTRES from a VM's disk image to DEST (no review page).
FAT is read directly; NTFS goes through ntfsread.py (ntfs-3g in WSL)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from review import Fat, Disk, vm_disk
try:
    fat = Fat(Disk(vm_disk(sys.argv[1])))
except SystemExit:
    import ntfsread
    ntfsread.copy_tree(vm_disk(sys.argv[1]), 'CTRES', sys.argv[2])
    print(sys.argv[2])
    sys.exit(0)
c = fat.find('CTRES')
if c is None:
    raise SystemExit('no C:/CTRES')
fat.copy_tree(c, sys.argv[2])
print(sys.argv[2])
