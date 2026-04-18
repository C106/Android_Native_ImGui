#!/usr/bin/env python3
import argparse
import struct
import sys

sys.path.insert(0, "/tmp/ceserver_api")
from ceserver_api import CEServerClient


OFF = {
    "GWorld": 0x15772758,
    "PhysicsScene": 0xD90,
    "PhysSceneSceneCount": 0x4,
    "PhysSceneSceneIndexArray": 0x56,
    "GPhysXSceneMap": 0x15757948,
    "GPhysXSceneMapBuckets": 0x15757980,
    "GPhysXSceneMapBucketPtr": 0x15757988,
    "GPhysXSceneMapHashSize": 0x15757990,
    "PxSceneActors": 0x2568,
    "PxSceneActorCount": 0x2570,
    "PxActorShapes": 0x28,
    "PxActorShapeCount": 0x30,
    "PxShapeFlags": 0x38,
    "PxShapeCorePtr": 0x40,
    "PxShapeGeometryInline": 0x98,
    "PxShapeCoreGeometry": 0x38,
}

PX_GEOM_TRIANGLE_MESH = 5


def read_u64(ce, addr):
    try:
        value = ce.read_uint64(addr)
        return 0 if value is None else value
    except Exception:
        return 0


def read_u32(ce, addr):
    try:
        value = ce.read_uint32(addr)
        return 0 if value is None else value
    except Exception:
        return 0


def read_u16(ce, addr):
    try:
        value = ce.read_uint16(addr)
        return 0 if value is None else value
    except Exception:
        return 0


def read_i32(ce, addr):
    try:
        value = ce.read_int32(addr)
        return -1 if value is None else value
    except Exception:
        return -1


def read_bytes(ce, addr, size):
    try:
        value = ce.read_bytes(addr, size)
        return b"" if value is None else value
    except Exception:
        return b""


def unpack_qwords_partial(raw):
    if raw is None:
        return ()
    usable = len(raw) & ~0x7
    if usable <= 0:
        return ()
    if usable != len(raw):
        raw = raw[:usable]
    return struct.unpack("<" + "Q" * (usable // 8), raw)


def read_qword_array_chunked(ce, addr, count, chunk_qwords=64):
    out = []
    missing = 0
    offset = 0
    while offset < count:
        take = min(chunk_qwords, count - offset)
        raw = read_bytes(ce, addr + offset * 8, take * 8)
        vals = unpack_qwords_partial(raw)
        if len(vals) == take:
            out.extend(vals)
            offset += take
            continue

        # Bulk reads through ceserver are sometimes truncated. Fall back to
        # per-qword reads so the scan can continue through the whole array.
        if vals:
            out.extend(vals)
        recovered = len(vals)
        for inner in range(recovered, take):
            value = read_u64(ce, addr + (offset + inner) * 8)
            if value == 0:
                missing += 1
            out.append(value)
        offset += take
    return out, missing


def find_pid(ce, process_name):
    name_l = process_name.lower()
    for proc in ce.enumerate_processes():
        if name_l in proc.name.lower():
            return proc.pid, proc.name
    return 0, ""


def find_libue4_base(ce):
    for mod in ce.enumerate_modules():
        if "libUE4.so" in mod.name:
            return mod.base, mod.name
    return 0, ""


def lookup_px_scene(ce, libue4, scene_index):
    hash_size = read_u32(ce, libue4 + OFF["GPhysXSceneMapHashSize"])
    entry_array = read_u64(ce, libue4 + OFF["GPhysXSceneMap"])
    bucket_base = read_u64(ce, libue4 + OFF["GPhysXSceneMapBucketPtr"])
    if bucket_base == 0:
        bucket_base = libue4 + OFF["GPhysXSceneMapBuckets"]
    if hash_size == 0 or entry_array == 0 or bucket_base == 0:
        return 0

    bucket = read_i32(ce, bucket_base + 4 * ((hash_size - 1) & scene_index))
    while bucket != -1:
        raw = read_bytes(ce, entry_array + bucket * 0x18, 0x18)
        if len(raw) < 0x18:
            return 0
        key = struct.unpack_from("<H", raw, 0x0)[0]
        scene = struct.unpack_from("<Q", raw, 0x8)[0]
        next_idx = struct.unpack_from("<i", raw, 0x10)[0]
        if key == scene_index:
            return scene
        bucket = next_idx
    return 0


def collect_scenes(ce, libue4, uworld):
    phys_scene = read_u64(ce, uworld + OFF["PhysicsScene"])
    scene_count = read_u32(ce, phys_scene + OFF["PhysSceneSceneCount"]) if phys_scene else 0
    out = []
    for i in range(scene_count):
        scene_index = read_u16(ce, phys_scene + OFF["PhysSceneSceneIndexArray"] + i * 2)
        px_scene = lookup_px_scene(ce, libue4, scene_index)
        actor_count = read_u32(ce, px_scene + OFF["PxSceneActorCount"]) if px_scene else 0
        actors_ptr = read_u64(ce, px_scene + OFF["PxSceneActors"]) if px_scene else 0
        out.append((i, scene_index, px_scene, actor_count, actors_ptr))
    return phys_scene, scene_count, out


def find_triangle_mesh_sample(ce, px_scene):
    actor_count = read_u32(ce, px_scene + OFF["PxSceneActorCount"])
    actors_ptr = read_u64(ce, px_scene + OFF["PxSceneActors"])
    if actor_count == 0 or actors_ptr == 0:
        return None

    actors, actor_missing = read_qword_array_chunked(ce, actors_ptr, actor_count)
    if not actors:
        return None

    scanned_actors = 0
    for actor_idx, actor in enumerate(actors):
        scanned_actors += 1
        if actor == 0:
            continue
        shape_count = read_u16(ce, actor + OFF["PxActorShapeCount"])
        if shape_count == 0 or shape_count > 64:
            continue

        shapes_handle = read_u64(ce, actor + OFF["PxActorShapes"])
        if shapes_handle == 0:
            continue

        if shape_count == 1:
            shapes = [shapes_handle]
        else:
            shapes, shape_missing = read_qword_array_chunked(ce, shapes_handle, shape_count, chunk_qwords=16)
            if not shapes:
                continue

        for shape_idx, shape in enumerate(shapes):
            if shape == 0:
                continue
            np_shape_flags = read_u32(ce, shape + OFF["PxShapeFlags"])
            geometry_addr = shape + OFF["PxShapeGeometryInline"]
            if np_shape_flags & 1:
                core = read_u64(ce, shape + OFF["PxShapeCorePtr"])
                if core == 0:
                    continue
                geometry_addr = core + OFF["PxShapeCoreGeometry"]
            geometry_type = read_u32(ce, geometry_addr)
            if geometry_type != PX_GEOM_TRIANGLE_MESH:
                continue

            geometry_raw = read_bytes(ce, geometry_addr, 0x60)
            if len(geometry_raw) < 0x30:
                continue
            mesh_ptr_20 = read_u64(ce, geometry_addr + 0x20)
            mesh_ptr_28 = read_u64(ce, geometry_addr + 0x28)
            mesh_ptr_30 = read_u64(ce, geometry_addr + 0x30)
            mesh_ptr_38 = read_u64(ce, geometry_addr + 0x38)
            mesh_ptr_40 = read_u64(ce, geometry_addr + 0x40)
            mesh_addr = mesh_ptr_28 or mesh_ptr_30
            mesh_raw = read_bytes(ce, mesh_addr, 0x120) if mesh_addr else b""
            return {
                "actor_idx": actor_idx,
                "actor": actor,
                "shape_idx": shape_idx,
                "shape": shape,
                "shape_count": shape_count,
                "np_shape_flags": np_shape_flags,
                "geometry_addr": geometry_addr,
                "geometry_raw": geometry_raw,
                "actors_requested": actor_count,
                "actors_returned": len(actors),
                "actors_missing": actor_missing,
                "shapes_missing": shape_missing if shape_count != 1 else 0,
                "geom_ptr_20": mesh_ptr_20,
                "geom_ptr_28": mesh_ptr_28,
                "geom_ptr_30": mesh_ptr_30,
                "geom_ptr_38": mesh_ptr_38,
                "geom_ptr_40": mesh_ptr_40,
                "mesh_addr": mesh_addr,
                "mesh_raw": mesh_raw,
            }
    return {
        "actors_requested": actor_count,
        "actors_returned": len(actors),
        "actors_missing": actor_missing,
        "actors_scanned": scanned_actors,
    }


def main():
    parser = argparse.ArgumentParser(description="Re-read live GWorld/PhysX/TriangleMesh after map reload")
    parser.add_argument("--host", default="192.168.31.235")
    parser.add_argument("--port", type=int, default=52736)
    parser.add_argument("--process", default="com.tencent.tmgp.pubgmhd")
    args = parser.parse_args()

    with CEServerClient(args.host, args.port) as ce:
        pid, process_name = find_pid(ce, args.process)
        if pid == 0:
            raise SystemExit(f"process not found: {args.process}")
        ce.pid = pid
        ce.open_process()

        libue4, lib_name = find_libue4_base(ce)
        if libue4 == 0:
            raise SystemExit("libUE4.so not found")

        uworld = read_u64(ce, libue4 + OFF["GWorld"])
        phys_scene, scene_count, scenes = collect_scenes(ce, libue4, uworld)

        print(f"process={process_name} pid={pid}")
        print(f"libUE4={lib_name} base={hex(libue4)}")
        print(f"GWorldAddr={hex(libue4 + OFF['GWorld'])} value={hex(uworld)}")
        print(f"PhysicsSceneAddr={hex(uworld + OFF['PhysicsScene']) if uworld else '0x0'} value={hex(phys_scene)}")
        print(f"PhysSceneSceneCount={scene_count}")

        live_scenes = []
        for slot, scene_index, px_scene, actor_count, actors_ptr in scenes:
            print(
                f"sceneSlot={slot} sceneIndex={scene_index} pxScene={hex(px_scene)} "
                f"actorCount={actor_count} actorsPtr={hex(actors_ptr)}"
            )
            if px_scene and actor_count and actors_ptr:
                live_scenes.append((actor_count, slot, scene_index, px_scene))

        if not live_scenes:
            raise SystemExit("no live px scene")

        live_scenes.sort(reverse=True)
        actor_count, slot, scene_index, px_scene = live_scenes[0]
        print(f"chosenSceneSlot={slot} chosenSceneIndex={scene_index} chosenPxScene={hex(px_scene)} actorCount={actor_count}")

        sample = find_triangle_mesh_sample(ce, px_scene)
        if not sample or "geometry_addr" not in sample:
            if sample:
                print(
                    f"triangleMeshSample=none actorsRead={sample['actors_returned']}/{sample['actors_requested']} "
                    f"actorsMissing={sample['actors_missing']} actorsScanned={sample['actors_scanned']}"
                )
            else:
                print("triangleMeshSample=none")
            return

        print(
            f"triangleMesh actorIdx={sample['actor_idx']} actor={hex(sample['actor'])} "
            f"shapeIdx={sample['shape_idx']} shape={hex(sample['shape'])} shapeCount={sample['shape_count']} "
            f"actorsRead={sample['actors_returned']}/{sample['actors_requested']} "
            f"actorsMissing={sample['actors_missing']} shapesMissing={sample['shapes_missing']}"
        )
        print(
            f"npShapeFlags={hex(sample['np_shape_flags'])} geometryAddr={hex(sample['geometry_addr'])} "
            f"geometryRaw={sample['geometry_raw'].hex()}"
        )
        print(f"geom+0x20={hex(sample['geom_ptr_20'])}")
        print(f"geom+0x28={hex(sample['geom_ptr_28'])}")
        print(f"geom+0x30={hex(sample['geom_ptr_30'])}")
        print(f"geom+0x38={hex(sample['geom_ptr_38'])}")
        print(f"geom+0x40={hex(sample['geom_ptr_40'])}")
        print(f"meshAddr={hex(sample['mesh_addr'])}")
        print(f"meshRaw={sample['mesh_raw'].hex()}")


if __name__ == "__main__":
    main()
