/* Read-only FBX verification using the exact ufbx source shipped with MyEngine. */
#include "ufbx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void bounds(const ufbx_node *node, double lo[3], double hi[3], int skinned)
{
    const ufbx_mesh *mesh = node->mesh;
    const ufbx_vertex_vec3 *positions = skinned ? &mesh->skinned_position : &mesh->vertex_position;
    for (size_t i = 0; i < positions->values.count; ++i) {
        ufbx_vec3 p = positions->values.data[i];
        if (!skinned || mesh->skinned_is_local) {
            p = ufbx_transform_position(&node->geometry_to_world, p);
        }
        double v[3] = { p.x, p.y, p.z };
        for (int k = 0; k < 3; ++k) {
            check(isfinite(v[k]), "Nonfinite position");
            if (v[k] < lo[k]) lo[k] = v[k];
            if (v[k] > hi[k]) hi[k] = v[k];
        }
    }
}

int main(int argc, char **argv)
{
    for (int arg = 1; arg < argc; ++arg) {
        ufbx_load_opts opts = {0};
        opts.target_axes = ufbx_axes_left_handed_y_up;
        opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;
        opts.target_unit_meters = 1.0;
        opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
        opts.generate_missing_normals = true;
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
        ufbx_error err;
        ufbx_scene *scene = ufbx_load_file(argv[arg], &opts, &err);
        if (!scene) {
            fprintf(stderr, "FAIL: %s: %s\n", argv[arg], err.description.data);
            failures++;
            continue;
        }
        size_t triangles = 0;
        double lo[3] = {1e30,1e30,1e30}, hi[3] = {-1e30,-1e30,-1e30};
        printf("FILE %s\n", argv[arg]);
        check(scene->meshes.count > 0, "No meshes");
        for (size_t n = 0; n < scene->nodes.count; ++n) {
            const ufbx_node *node = scene->nodes.data[n];
            if (node->mesh) bounds(node, lo, hi, 0);
        }
        for (size_t m = 0; m < scene->meshes.count; ++m) {
            const ufbx_mesh *mesh = scene->meshes.data[m];
            triangles += mesh->num_triangles;
            check(mesh->vertex_uv.exists, "Missing UV0");
            check(mesh->uv_sets.count == 1, "Expected one UV set");
            check(mesh->vertex_normal.exists, "Missing normals");
            check(mesh->num_faces == mesh->num_triangles, "Non-triangle mesh");
            for (size_t i = 0; i < mesh->vertex_uv.values.count; ++i) {
                ufbx_vec2 uv = mesh->vertex_uv.values.data[i];
                check(isfinite(uv.x) && isfinite(uv.y), "Nonfinite UV");
            }
            for (size_t s = 0; s < mesh->skin_deformers.count; ++s) {
                const ufbx_skin_deformer *skin = mesh->skin_deformers.data[s];
                check(skin->clusters.count <= 128, "Too many bones");
                for (size_t v = 0; v < skin->vertices.count; ++v) {
                    ufbx_skin_vertex sv = skin->vertices.data[v];
                    check(sv.num_weights > 0 && sv.num_weights <= 4, "Invalid weight count");
                    double total = 0;
                    for (size_t w = 0; w < sv.num_weights; ++w)
                        total += skin->weights.data[sv.weight_begin+w].weight;
                    check(fabs(total-1.0) < 1e-5, "Weights not normalized");
                }
            }
        }
        printf("  triangles=%zu meshes=%zu bounds=[%.4f %.4f %.4f]..[%.4f %.4f %.4f]\n",
               triangles, scene->meshes.count, lo[0],lo[1],lo[2],hi[0],hi[1],hi[2]);
        for (size_t m = 0; m < scene->materials.count; ++m) {
            const ufbx_material *mat = scene->materials.data[m];
            check(mat->pbr.base_color.texture != NULL || mat->fbx.diffuse_color.texture != NULL,
                  "Missing base color texture connection");
            check(mat->pbr.normal_map.texture != NULL || mat->fbx.normal_map.texture != NULL,
                  "Missing normal texture connection");
            check(mat->pbr.metalness.has_value, "Missing metallic scalar");
            check(mat->pbr.roughness.has_value, "Missing roughness scalar");
            check(fabs(mat->pbr.metalness.value_real - (strcmp(mat->name.data,"Steel")==0 ? .9 : 0)) < .001,
                  "Metallic value does not match the approved material");
            check(fabs(mat->pbr.base_color.value_vec3.x-1)<1e-5 &&
                  fabs(mat->pbr.base_color.value_vec3.y-1)<1e-5 &&
                  fabs(mat->pbr.base_color.value_vec3.z-1)<1e-5, "Unexpected base color multiplier");
            double expected_opacity = strcmp(mat->name.data,"Glass")==0 ? .58 :
                                      strcmp(mat->name.data,"Water")==0 ? .55 : 1;
            check(mat->pbr.opacity.has_value && fabs(mat->pbr.opacity.value_real-expected_opacity)<.001,
                  "Opacity does not match material");
            printf("  material=%s metal=%.3f rough=%.3f base=[%.3f %.3f %.3f] opacity=%.3f\n",
                   mat->name.data, mat->pbr.metalness.value_real,mat->pbr.roughness.value_real,
                   mat->pbr.base_color.value_vec3.x,mat->pbr.base_color.value_vec3.y,
                   mat->pbr.base_color.value_vec3.z,mat->pbr.opacity.value_real);
        }
        for (size_t t = 0; t < scene->textures.count; ++t) {
            const ufbx_texture *tex = scene->textures.data[t];
            if (tex->type != UFBX_TEXTURE_FILE) continue;
            FILE *fp = fopen(tex->filename.data, "rb");
            if (!fp) fprintf(stderr, "Missing texture: %s\n", tex->filename.data);
            check(fp != NULL, "Texture path does not resolve");
            if (fp) fclose(fp);
        }
        for (size_t a = 0; a < scene->anim_stacks.count; ++a) {
            const ufbx_anim_stack *stack = scene->anim_stacks.data[a];
            ufbx_bake_opts bake_opts = {0};
            bake_opts.resample_rate = 60;
            bake_opts.minimum_sample_rate = 60;
            bake_opts.trim_start_time = true;
            ufbx_baked_anim *anim = ufbx_bake_anim(scene, stack->anim, &bake_opts, &err);
            check(anim != NULL, "Animation bake failed");
            if (anim) {
                printf("  clip=%s duration=%.6f nodes=%zu\n", stack->name.data,
                       anim->playback_duration,anim->nodes.count);
                check(fabs(anim->playback_duration-1.0) < 1e-5, "Door clip must be 1 second");
                ufbx_free_baked_anim(anim);
            }
            ufbx_evaluate_opts eval_opts = {0};
            eval_opts.evaluate_skinning = true;
            double widths[2] = {0}, depths[2] = {0};
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                const double time = endpoint ? stack->time_end : stack->time_begin;
                ufbx_scene *pose = ufbx_evaluate_scene(scene, stack->anim,time,&eval_opts,&err);
                check(pose != NULL, "Door pose evaluation failed");
                if (!pose) continue;
                for (size_t n = 0; n < pose->nodes.count; ++n) {
                    const ufbx_node *node = pose->nodes.data[n];
                    if (!node->mesh || !strstr(node->name.data, "leaf")) continue;
                    double pmin[3]={1e30,1e30,1e30},pmax[3]={-1e30,-1e30,-1e30};
                    bounds(node,pmin,pmax,1);
                    widths[endpoint]=pmax[0]-pmin[0];
                    depths[endpoint]=pmax[2]-pmin[2];
                    printf("    endpoint=%d leaf_width=%.4f depth=%.4f\n",
                           endpoint,widths[endpoint],depths[endpoint]);
                }
                ufbx_free_scene(pose);
            }
            check(fabs(widths[0]-depths[1])<.02 && fabs(widths[1]-depths[0])<.02,
                  "Door must rotate 90 degrees");
            check(fabs(widths[0]-widths[1])>1.0,"Door leaf did not move");
        }
        if (strstr(argv[arg],"Lab_Door")) check(scene->anim_stacks.count==2,"Expected two door clips");
        ufbx_free_scene(scene);
    }
    printf("RESULT: %d failure(s)\n",failures);
    return failures ? 1 : 0;
}
