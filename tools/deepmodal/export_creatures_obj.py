"""
Thingi10K extraction script specifically for Character / Enemy / Creature models.
Takes creature_list.txt and extracts clean manifold OBJ files.
"""

import os
import sys
import argparse
from pathlib import Path
import numpy as np

def main():
    parser = argparse.ArgumentParser(description="Extract creature/enemy models from Thingi10K")
    parser.add_argument("--list", type=str, default="tools/deepmodal/creature_list.txt", help="Input list of file_ids")
    parser.add_argument("--limit", type=int, default=1000, help="Max models to export")
    parser.add_argument("--out", type=str, default="tools/deepmodal/data/raw/creatures", help="Output directory")
    parser.add_argument("--out-list", type=str, default="tools/deepmodal/creatures_obj_list.txt", help="Output list path")
    args = parser.parse_args()

    list_path = Path(args.list)
    if not list_path.exists():
        print(f"Error: {list_path} not found")
        sys.exit(1)

    target_ids = set()
    with open(list_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                target_ids.add(int(line))

    print(f"[Creatures] Target file IDs loaded: {len(target_ids)}")

    out_dir = Path(args.out)
    obj_dir = out_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    out_list_path = Path(args.out_list)

    print("[Creatures] Initializing Thingi10K dataset...")
    import thingi10k
    thingi10k.init(variant="npz")

    ds = thingi10k.dataset(
        manifold=True,
        closed=True,
        solid=True,
        num_components=1,
    )

    exported_files = []
    skipped = 0

    print(f"[Creatures] Filtering and exporting up to {args.limit} models to {obj_dir}...")

    count = 0
    for i in range(len(ds)):
        row = ds[i]
        file_id = row["file_id"]
        if file_id not in target_ids:
            continue

        obj_name = f"{file_id}.obj"
        target_obj = obj_dir / obj_name

        if target_obj.exists() and target_obj.stat().st_size > 0:
            exported_files.append(str(target_obj.resolve()))
            count += 1
            if count >= args.limit:
                break
            continue

        try:
            mesh_data = thingi10k.load_file(row["file_path"])
            if isinstance(mesh_data, tuple):
                vertices, facets = mesh_data[0], mesh_data[1]
            elif isinstance(mesh_data, dict):
                vertices, facets = mesh_data["vertices"], mesh_data["facets"]
            else:
                vertices, facets = mesh_data.vertices, mesh_data.facets

            if len(vertices) == 0 or len(facets) == 0:
                skipped += 1
                continue

            with open(target_obj, "w", encoding="utf-8") as f:
                f.write(f"# Thingi10K Creature file_id: {file_id}\n")
                f.write(f"# vertices: {len(vertices)}, facets: {len(facets)}\n")
                for v in vertices:
                    f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
                for face in facets:
                    f.write(f"f {face[0]+1} {face[1]+1} {face[2]+1}\n")

            exported_files.append(str(target_obj.resolve()))
            count += 1

            if len(exported_files) % 100 == 0:
                print(f"  Exported {len(exported_files)}/{args.limit} creature models...")

            if count >= args.limit:
                break

        except Exception as e:
            print(f"  [Warning] Failed {file_id}: {e}")
            skipped += 1

    print(f"[Creatures] Successfully exported {len(exported_files)} creature OBJ models (skipped: {skipped})")

    with open(out_list_path, "w", encoding="utf-8") as f:
        for p in exported_files:
            f.write(f"{p}\n")

    print(f"[Creatures] Saved file list to {out_list_path.resolve()} ({len(exported_files)} lines)")

if __name__ == "__main__":
    main()
