#!/usr/bin/env python3
# PhysX BVH 验证工具 - 从 GWorld 遍历到 TriangleMesh 并验证 BVH
import socket, struct, sys

CE_HOST, CE_PORT = '192.168.31.235', 52736

class CE:
    def __init__(self):
        self.s = socket.socket()
        self.s.settimeout(5)
        self.s.connect((CE_HOST, CE_PORT))
        print(f"[+] Connected to CE server")

    def cmd(self, c):
        self.s.sendall((c + '\n').encode())
        return self.s.recv(8192).decode('utf-8', errors='ignore').strip()

    def open_proc(self, name):
        for line in self.cmd('getprocesslist').split('\n'):
            if name.lower() in line.lower():
                pid = line.split('\t')[0]
                self.cmd(f'openprocess {pid}')
                print(f"[+] Opened: {line.split(chr(9))[1]}")
                return True
        return False

    def r64(self, a):
        try: return int.from_bytes(bytes.fromhex(self.cmd(f'readmemory {a:x} 8').replace(' ','')), 'little')
        except: return 0

    def r32(self, a):
        try: return int.from_bytes(bytes.fromhex(self.cmd(f'readmemory {a:x} 4').replace(' ','')), 'little')
        except: return 0

def find_mesh_from_gworld(ce, gworld, libue4_base):
    """从 GWorld 遍历到 TriangleMesh"""
    print(f"\n[*] GWorld: 0x{gworld:x}")
    
    phys_scene = ce.r64(gworld + 0xD88)
    print(f"  PhysicsScene: 0x{phys_scene:x}")
    if phys_scene == 0:
        return None
    
    scene_idx = ce.r32(phys_scene + 0x56) & 0xFFFF
    print(f"  SceneIndex: {scene_idx}")
    
    px_scene = ce.r64(libue4_base + 0x1496E950 + scene_idx * 8)
    print(f"  PxScene: 0x{px_scene:x}")
    if px_scene == 0:
        return None
    
    actors_ptr = ce.r64(px_scene + 0x2568)
    actor_count = ce.r32(px_scene + 0x2570)
    print(f"  Actors: {actor_count}")
    
    if actor_count == 0 or actor_count > 10000:
        return None
    
    # 遍历查找 TriangleMesh
    for i in range(min(20, actor_count)):
        actor = ce.r64(actors_ptr + i * 8)
        if actor == 0:
            continue
        
        shapes_ptr = ce.r64(actor + 0x28)
        if shapes_ptr == 0:
            continue
        
        shape = ce.r64(shapes_ptr)
        if shape == 0:
            continue
        
        geom_type = ce.r32(shape + 0x98)
        if geom_type == 5:  # TriangleMesh
            mesh = ce.r64(shape + 0x98 + 0x28)
            if mesh != 0:
                return mesh
    
    return None


def verify_mesh(ce, mesh):
    """验证 TriangleMesh 和 BVH"""
    print(f"\n[*] Verifying mesh at 0x{mesh:x}")
    
    vtable = ce.r64(mesh)
    nb_v = ce.r32(mesh + 0x1C)
    nb_t = ce.r32(mesh + 0x20)
    v_ptr = ce.r64(mesh + 0x28)
    t_ptr = ce.r64(mesh + 0x30)
    
    print(f"  vtable:    0x{vtable:x}")
    print(f"  vertices:  {nb_v} @ 0x{v_ptr:x}")
    print(f"  triangles: {nb_t} @ 0x{t_ptr:x}")
    
    if not (0 < nb_v < 100000 and 0 < nb_t < 100000):
        print("[-] Invalid mesh")
        return False
    
    print("[+] Valid mesh!")
    
    # 验证 BVH
    bvh_pages = ce.r64(mesh + 0xC0)
    bvh_nodes = ce.r32(mesh + 0xC8)
    bvh_pages_count = ce.r32(mesh + 0xCC)
    
    print(f"\n  BVH pages:  0x{bvh_pages:x}")
    print(f"  BVH nodes:  {bvh_nodes}")
    print(f"  BVH pages#: {bvh_pages_count}")
    
    if bvh_nodes > 0:
        print("[+] Mesh has BVH!")
        return True
    else:
        print("[-] No BVH data")
        return False


def main():
    ce = CE()
    if not ce.open_proc('app_process64 com.tencent.tmgp.pubgmhd'):
        sys.exit(1)
    
    # 模式选择
    print("\n[1] Auto: Find mesh from GWorld")
    print("[2] Manual: Verify specific mesh address")
    mode = input("Select mode (1/2): ").strip()
    
    if mode == '1':
        gworld = int(input("GWorld address (hex): "), 16)
        libue4 = int(input("libUE4.so base (hex): "), 16)
        
        mesh = find_mesh_from_gworld(ce, gworld, libue4)
        if mesh:
            print(f"\n[+] Found mesh: 0x{mesh:x}")
            verify_mesh(ce, mesh)
        else:
            print("[-] No mesh found")
    else:
        mesh = int(input("TriangleMesh address (hex): "), 16)
        verify_mesh(ce, mesh)

if __name__ == '__main__':
    main()
