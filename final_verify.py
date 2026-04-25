#!/usr/bin/env python3
import sys
sys.path.insert(0, "/tmp/ceserver_api")
from ceserver_api import CEServerClient

with CEServerClient('192.168.31.235', 52736) as ce:
    # 打开进程
    for p in ce.enumerate_processes():
        if 'pubgmhd' in p.name.lower():
            ce.pid = p.pid
            ce.open_process()
            print(f"[+] Process: {p.name}")
            break

    # 获取libUE4基址
    for m in ce.enumerate_modules():
        if 'libUE4' in m.name:
            base = m.base
            print(f"[+] libUE4: 0x{base:x}")
            break

    # 读取GWorld
    gworld = ce.read_uint64(base + 0x14988578)
    print(f"[+] GWorld: 0x{gworld:x}")

    # 读取PhysicsScene
    phys = ce.read_uint64(gworld + 0xD88)
    print(f"[+] PhysicsScene: 0x{phys:x}")

    # 读取SceneIndex
    idx = ce.read_uint16(phys + 0x56)
    print(f"[+] SceneIndex: {idx}")

    # LookupPxSceneByIndex
    hash_size = ce.read_uint32(base + 0x1496E960)
    entry_array = ce.read_uint64(base + 0x1496E958)
    bucket_ptr = ce.read_uint64(base + 0x1496E958)
    if bucket_ptr == 0:
        bucket_ptr = base + 0x1496E950
    
    print(f"[+] HashSize: {hash_size}")
    print(f"[+] BucketPtr: 0x{bucket_ptr:x}")
    
    # 查找PxScene
    bucket_idx = idx % hash_size if hash_size > 0 else 0
    entry = ce.read_uint64(bucket_ptr + bucket_idx * 8)
    
    px_scene = 0
    while entry != 0:
        key = ce.read_uint16(entry)
        if key == idx:
            px_scene = ce.read_uint64(entry + 0x8)
            break
        entry = ce.read_uint64(entry + 0x10)
    
    print(f"[+] PxScene: 0x{px_scene:x}")

    if px_scene == 0:
        print("[-] PxScene not found")
        sys.exit(1)
    
    # 读取actors
    actors_ptr = ce.read_uint64(px_scene + 0x2568)
    actor_count = ce.read_uint32(px_scene + 0x2570)
    print(f"[+] Actors: {actor_count}")
    
    # 查找TriangleMesh
    mesh = 0
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
        if geom_type == 5:
            mesh = ce.read_uint64(shape + 0x98 + 0x28)
            if mesh != 0:
                break

    if mesh == 0:
        print("[-] No mesh found")
        sys.exit(1)
    
    print(f"\n[+] TriangleMesh: 0x{mesh:x}")
    
    # 验证mesh
    nb_v = ce.read_uint32(mesh + 0x1C)
    nb_t = ce.read_uint32(mesh + 0x20)
    v_ptr = ce.read_uint64(mesh + 0x28)
    t_ptr = ce.read_uint64(mesh + 0x30)
    
    print(f"  Vertices:  {nb_v} @ 0x{v_ptr:x}")
    print(f"  Triangles: {nb_t} @ 0x{t_ptr:x}")
    
    # 验证BVH
    bvh_pages = ce.read_uint64(mesh + 0xC0)
    bvh_nodes = ce.read_uint32(mesh + 0xC8)
    
    print(f"\n[*] BVH:")
    print(f"  Pages: 0x{bvh_pages:x}")
    print(f"  Nodes: {bvh_nodes}")
    
    if bvh_nodes > 0:
        print("\n[SUCCESS] Mesh has valid BVH!")
    else:
        print("\n[-] No BVH")
