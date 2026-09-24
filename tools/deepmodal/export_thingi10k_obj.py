"""
Thingi10K download and extraction script for DeepModal dataset generation.
Filters for solid, closed, single-component, manifold meshes and converts them
to clean OBJ files ready for Editor.exe --modal-voxelize.
"""

import os
import sys
import argparse
from pathlib import Path
import numpy as np

def main():
    parser = argparse.ArgumentParser(description="Extract clean manifold OBJ models from Thingi10K")
    parser.add_argument("--limit", type=int, default=1000, help="Max number of models to extract (default: 1000)")
    parser.add_argument("--out", type=str, default="data/raw/thingi10k", help="Output directory for OBJ files")
    parser.add_argument("--min-facets", type=int, default=100, help="Min facets (default: 100)")
    parser.add_argument("--max-facets", type=int, default=50000, help="Max facets (default: 50000)")
    args = parser.parse_args()

    out_dir = Path(args.out)
    obj_dir = out_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    list_path = Path("thingi10k_list.txt")

    print("[Thingi10K] Initializing dataset (variant='npz')...")
    import thingi10k
    thingi10k.init(variant="npz")

    print(f"[Thingi10K] Querying dataset with filters:")
    print(f"  manifold=True, closed=True, solid=True, num_components=1")
    print(f"  facets=[{args.min_facets}, {args.max_facets}], limit={args.limit}")

    ds = thingi10k.dataset(
        manifold=True,
        closed=True,
        solid=True,
        num_components=1,
        num_facets=(args.min_facets, args.max_facets),
    )

    total_matched = len(ds)
    print(f"[Thingi10K] Total matching models in dataset: {total_matched}")

    num_to_process = min(args.limit, total_matched)
    print(f"[Thingi10K] Extracting {num_to_process} models to {obj_dir}...")

    exported_files = []
    skipped = 0

    for i in range(num_to_process):
        row = ds[i]
        file_id = row["file_id"]
        obj_name = f"{file_id}.obj"
        target_obj = obj_dir / obj_name

        if target_obj.exists() and target_obj.stat().st_size > 0:
            exported_files.append(str(target_obj.resolve()))
            continue

        try:
            # load_file returns (vertices, facets) as numpy arrays
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

            # Write standard Wavefront OBJ
            with open(target_obj, "w", encoding="utf-8") as f:
                f.write(f"# Thingi10K file_id: {file_id}\n")
                f.write(f"# vertices: {len(vertices)}, facets: {len(facets)}\n")
                for v in vertices:
                    f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
                # OBJ indices are 1-based
                for face in facets:
                    f.write(f"f {face[0]+1} {face[1]+1} {face[2]+1}\n")

            exported_files.append(str(target_obj.resolve()))

            if (len(exported_files) % 100) == 0:
                print(f"  Processed {len(exported_files)}/{num_to_process} models...")

        except Exception as e:
            print(f"  [Warning] Failed to process {file_id}: {e}")
            skipped += 1

    print(f"[Thingi10K] Successfully exported {len(exported_files)} OBJ models (skipped: {skipped})")

    # Write list for Editor.exe --modal-voxelize
    with open(list_path, "w", encoding="utf-8") as f:
        for p in exported_files:
            f.write(f"{p}\n")

    print(f"[Thingi10K] Saved voxelizer file list to {list_path.resolve()} ({len(exported_files)} lines)")

if __name__ == "__main__":
    main()
