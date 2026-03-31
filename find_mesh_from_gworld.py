#!/usr/bin/env python3
# 从 GWorld 遍历到 PxTriangleMesh

import socket
import struct

CE_HOST = '192.168.31.235'
CE_PORT = 52736

class CE:
    def __init__(self):
        self.s = socket.socket()
        self.s.settimeout(5)
        self.s.connect((CE_HOST, CE_PORT))

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

def find_gworld(ce):
    """查找 GWorld 全局变量"""
    print("[*] Searching for GWorld...")
    # GWorld 通常在 .bss 段，需要扫描或使用已知偏移
    # 这里使用模块基址 + 偏移（需要从 IDA 获取）
    
    # 方法1: 扫描特征
    # 方法2: 使用已知偏移（从你的 mem_struct.h offset 结构）
    
    # 临时：让用户提供 GWorld 地址
    gworld_str = input("Enter GWorld address (hex): ")
    return int(gworld_str, 16)


def traverse_to_physx(ce, gworld):
    """从 GWorld 遍历到 PhysX Scene"""
    print(f"\n[*] GWorld: 0x{gworld:x}")
    
    # UWorld -> PhysicsScene (offset 需要从 IDA 确认)
    # 典型路径: UWorld -> FPhysScene -> PxScene
    
    physics_scene = ce.r64(gworld + 0x5B0)  # 示例偏移
    print(f"  PhysicsScene: 0x{physics_scene:x}")
    
    if physics_scene == 0:
        print("[-] PhysicsScene is null")
        return None
    
    # FPhysScene -> PxScene
    px_scene = ce.r64(physics_scene + 0x10)  # 示例偏移
    print(f"  PxScene: 0x{px_scene:x}")
    
    return px_scene


def find_triangle_meshes(ce, px_scene):
    """从 PxScene 查找 TriangleMesh"""
    print(f"\n[*] Searching meshes in PxScene: 0x{px_scene:x}")
    
    # PxScene 包含 actors 数组
    # 遍历 actors -> shapes -> geometry -> TriangleMesh
    
    # 简化：直接扫描内存中的 vtable 特征
    # RTreeTriangleMesh vtable: 0x1a0a261 (从 IDA)
    # BV4TriangleMesh vtable: 0x1a09c55
    
    print("[*] Scanning for TriangleMesh vtables...")
    
    # 这里需要模块基址
    base_str = input("Enter libUE4.so base address (hex): ")
    base = int(base_str, 16)
    
    rtree_vtable = base + 0x1a0a261
    bv4_vtable = base + 0x1a09c55
    
    print(f"  RTree vtable: 0x{rtree_vtable:x}")
    print(f"  BV4 vtable:   0x{bv4_vtable:x}")
    
    return rtree_vtable, bv4_vtable


def verify_mesh(ce, addr):
    """验证并输出 mesh 信息"""
    print(f"\n[*] Verifying mesh at 0x{addr:x}")
    
    vtable = ce.r64(addr)
    nb_verts = ce.r32(addr + 0x28)
    nb_tris = ce.r32(addr + 0x2C)
    verts_ptr = ce.r64(addr + 0x30)
    tris_ptr = ce.r64(addr + 0x38)
    
    print(f"  vtable:      0x{vtable:x}")
    print(f"  nbVertices:  {nb_verts}")
    print(f"  nbTriangles: {nb_tris}")
    print(f"  vertices:    0x{verts_ptr:x}")
    print(f"  triangles:   0x{tris_ptr:x}")
    
    if 0 < nb_verts < 100000 and 0 < nb_tris < 100000:
        print("[+] Valid mesh!")
        
        # 检查 BVH
        rtree_pages = ce.r64(addr + 0xC0)
        rtree_nodes = ce.r32(addr + 0xC8)
        print(f"\n  BVH pages:   0x{rtree_pages:x}")
        print(f"  BVH nodes:   {rtree_nodes}")
        
        if rtree_nodes > 0:
            print("[+] Mesh has BVH!")
        return True
    return False

def main():
    ce = CE()
    if not ce.open_proc('pubgmhd'):
        return
    
    gworld = find_gworld(ce)
    px_scene = traverse_to_physx(ce, gworld)
    
    if px_scene:
        find_triangle_meshes(ce, px_scene)
    
    # 让用户输入找到的 mesh 地址验证
    mesh_str = input("\nEnter TriangleMesh address to verify (hex): ")
    mesh_addr = int(mesh_str, 16)
    verify_mesh(ce, mesh_addr)

if __name__ == '__main__':
    main()
