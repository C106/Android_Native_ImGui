#!/usr/bin/env python3
import sys
sys.path.insert(0, "/tmp/ceserver_api")
from ceserver_api import CEServerClient

HOST = '192.168.31.235'
PORT = 52736

# 连接并找到进程
with CEServerClient(HOST, PORT) as ce:
    procs = ce.enumerate_processes()
    target_pid = None

    for p in procs:
        if 'pubgmhd' in p.name.lower():
            target_pid = p.pid
            print(f"[+] Found: {p.name} (PID: {target_pid})")
            break

    if not target_pid:
        print("[-] Process not found")
        sys.exit(1)

    ce.pid = target_pid
    ce.open_process()

    # 获取 libUE4.so 基址
    libue4_base = None
    for m in ce.enumerate_modules():
        if 'libUE4' in m.name:
            libue4_base = m.base
            print(f"[+] libUE4.so base: 0x{libue4_base:x}")
            break

    if not libue4_base:
        print("[-] libUE4.so not found")
        sys.exit(1)

    # 读取 GWorld
    gworld_off = 0x14988578  # 需要从你的代码中获取
    gworld = ce.read_uint64(libue4_base + gworld_off)
    print(f"[*] GWorld: 0x{gworld:x}")

    if gworld == 0:
        print("[-] GWorld is null")
        sys.exit(1)

    # UWorld -> PhysicsScene
    phys_scene = ce.read_uint64(gworld + 0xD88)
    print(f"[*] PhysicsScene: 0x{phys_scene:x}")

    if phys_scene == 0:
        print("[-] PhysicsScene is null")
        sys.exit(1)

    # FPhysScene -> SceneIndex -> PxScene
    scene_idx = ce.read_uint16(phys_scene + 0x56)
    print(f"[*] SceneIndex: {scene_idx}")

    px_scene = ce.read_uint64(libue4_base + 0x1496E950 + scene_idx * 8)
    print(f"[*] PxScene: 0x{px_scene:x}")

    if px_scene == 0:
        print("[-] PxScene is null")
        sys.exit(1)

    # PxScene -> Actors
    actors_ptr = ce.read_uint64(px_scene + 0x2568)
    actor_count = ce.read_uint32(px_scene + 0x2570)
    print(f"[*] Actors: {actor_count}")

    # 遍历查找 TriangleMesh
    mesh_addr = None
    for i in range(min(20, actor_count)):
        actor = ce.read_uint64(actors_ptr + i * 8)
        if actor == 0:
            continue

        shapes_ptr = ce.read_uint64(actor + 0x28)
        if shapes_ptr == 0:
            continue

        shape = ce.read_uint64(shapes_ptr)
        if shape == 0:
            continue

        geom_type = ce.read_uint32(shape + 0x98)
        if geom_type == 5:  # TriangleMesh
            mesh_addr = ce.read_uint64(shape + 0x98 + 0x28)
            if mesh_addr != 0:
                print(f"\n[+] Found TriangleMesh: 0x{mesh_addr:x}")
                break

    if not mesh_addr:
        print("[-] No TriangleMesh found")
        sys.exit(1)

    # 验证 mesh 结构
    nb_verts = ce.read_uint32(mesh_addr + 0x1C)
    nb_tris = ce.read_uint32(mesh_addr + 0x20)
    verts_ptr = ce.read_uint64(mesh_addr + 0x28)
    tris_ptr = ce.read_uint64(mesh_addr + 0x30)

    print(f"  Vertices:  {nb_verts} @ 0x{verts_ptr:x}")
    print(f"  Triangles: {nb_tris} @ 0x{tris_ptr:x}")

    if 0 < nb_verts < 100000 and 0 < nb_tris < 100000:
        print("[+] Valid mesh structure!")
    else:
        print("[-] Invalid mesh")
        sys.exit(1)

    # 验证 BVH (offset +0xC0)
    bvh_pages = ce.read_uint64(mesh_addr + 0xC0)
    bvh_nodes = ce.read_uint32(mesh_addr + 0xC8)
    bvh_pages_count = ce.read_uint32(mesh_addr + 0xCC)

    print(f"\n[*] BVH Structure:")
    print(f"  Pages:  0x{bvh_pages:x}")
    print(f"  Nodes:  {bvh_nodes}")
    print(f"  Pages#: {bvh_pages_count}")

    if bvh_nodes > 0:
        print("\n[+] SUCCESS: Mesh has valid BVH!")
    else:
        print("\n[-] No BVH data found")
