"""pullfile.py VMDIR GUESTDIR DEST -- copy a folder tree (e.g. ZDBENCH) out of a VM's C: drive."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from review import Fat, Disk, vm_disk
fat = Fat(Disk(vm_disk(sys.argv[1])))
c = fat.find(sys.argv[2].replace('/', chr(92)))
if c is None:
    raise SystemExit('not found: ' + sys.argv[2])
fat.copy_tree(c, sys.argv[3])
