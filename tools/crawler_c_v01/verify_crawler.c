/* Validate using MyEngine's ufbx implementation and coordinate conversion. */
#include "ufbx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int value,const char *reason) {
    if (!value) { fprintf(stderr,"FAIL: %s\n",reason); ++failures; }
}
static const char *names[]={"00_Idle","01_Patrol_Crawl","02_Alert","03_Search","04_Chase",
    "05_Attack","06_Hit","07_LightFlinch","08_Death","09_LightBoundary","10_WallCrawl"};
static const double seconds[]={3,2.4,1,3,.9,1.2,1,1.5,2,2.4,2};
static const int loops[]={1,1,0,1,1,0,0,0,0,1,1};

int main(int argc,char **argv) {
    if(argc!=2) return 2;
    ufbx_load_opts opts={0};
    opts.target_axes=ufbx_axes_left_handed_y_up;
    opts.handedness_conversion_axis=UFBX_MIRROR_AXIS_Z;
    opts.target_unit_meters=1;
    opts.space_conversion=UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.generate_missing_normals=true;
    opts.geometry_transform_handling=UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    ufbx_error error;
    ufbx_scene *scene=ufbx_load_file(argv[1],&opts,&error);
    if(!scene) { fprintf(stderr,"LOAD FAILED: %s\n",error.description.data); return 1; }
    check(scene->meshes.count==5,"Expected five material-separated skinned meshes");
    check(scene->anim_stacks.count==11,"Expected eleven animation clips");
    size_t triangles=0,clusters_max=0;
    for(size_t i=0;i<scene->meshes.count;i++) {
        const ufbx_mesh *m=scene->meshes.data[i];
        triangles+=m->num_triangles;
        check(m->num_triangles==m->num_faces,"Mesh not triangulated");
        check(m->vertex_normal.exists,"Missing normals");
        check(m->uv_sets.count==1 && m->vertex_uv.exists,"Expected one UV set");
        check(m->skin_deformers.count==1,"Missing or multiple skin deformers");
        for(size_t v=0;v<m->vertex_position.values.count;v++) {
            ufbx_vec3 p=m->vertex_position.values.data[v];
            check(isfinite(p.x)&&isfinite(p.y)&&isfinite(p.z),"Nonfinite vertex");
        }
        if(!m->skin_deformers.count) continue;
        const ufbx_skin_deformer *skin=m->skin_deformers.data[0];
        if(skin->clusters.count>clusters_max) clusters_max=skin->clusters.count;
        check(skin->clusters.count<120,"Insufficient space for ancestors in 128-bone palette");
        for(size_t v=0;v<skin->vertices.count;v++) {
            ufbx_skin_vertex sv=skin->vertices.data[v];
            check(sv.num_weights>0 && sv.num_weights<=4,"Invalid influence count");
            double sum=0;
            for(size_t w=0;w<sv.num_weights;w++) sum+=skin->weights.data[sv.weight_begin+w].weight;
            check(fabs(sum-1)<.00001,"Weights not normalized");
        }
    }
    for(size_t i=0;i<scene->materials.count;i++) {
        const ufbx_material *m=scene->materials.data[i];
        check(m->pbr.base_color.texture||m->fbx.diffuse_color.texture,"Missing color texture");
        check(m->pbr.normal_map.texture||m->fbx.normal_map.texture,"Missing normal texture");
        check(m->pbr.metalness.has_value && fabs(m->pbr.metalness.value_real)<.00001,"Skin should be nonmetallic");
        check(m->pbr.roughness.has_value,"Missing roughness");
        check(m->pbr.opacity.has_value && fabs(m->pbr.opacity.value_real-1)<.00001,"Unexpected opacity");
        printf("MATERIAL %s roughness=%.3f\n",m->name.data,m->pbr.roughness.value_real);
    }
    for(size_t i=0;i<scene->textures.count;i++) {
        const ufbx_texture *t=scene->textures.data[i];
        if(t->type!=UFBX_TEXTURE_FILE) continue;
        FILE *f=fopen(t->filename.data,"rb");
        check(f!=NULL,"Unresolved texture reference");
        if(f) fclose(f);
    }
    printf("GEOMETRY triangles=%zu meshes=%zu max_skin_clusters=%zu\n",triangles,scene->meshes.count,clusters_max);
    for(size_t i=0;i<scene->anim_stacks.count;i++) {
        const ufbx_anim_stack *stack=scene->anim_stacks.data[i];
        int index=-1;
        for(int j=0;j<11;j++) if(strstr(stack->name.data,names[j])) index=j;
        check(index>=0,"Unexpected clip name");
        if(index<0) continue;
        ufbx_bake_opts bake={0};
        bake.resample_rate=60;
        bake.minimum_sample_rate=60;
        bake.trim_start_time=true;
        ufbx_baked_anim *anim=ufbx_bake_anim(scene,stack->anim,&bake,&error);
        check(anim!=NULL,"Animation bake failed");
        if(anim) {
            check(fabs(anim->playback_duration-seconds[index])<.0001,"Incorrect clip duration");
            ufbx_free_baked_anim(anim);
        }
        ufbx_evaluate_opts evaluate={0};
        evaluate.evaluate_skinning=true;
        ufbx_scene *start=ufbx_evaluate_scene(scene,stack->anim,stack->time_begin,&evaluate,&error);
        check(start!=NULL,"Cannot evaluate initial pose");
        if(!start) continue;
        double maxmotion=0,enddiff=0,minheight=1e20,maxextent=0;
        for(int sample=0;sample<=8;sample++) {
            double t=stack->time_begin+(stack->time_end-stack->time_begin)*sample/8.;
            ufbx_scene *pose=ufbx_evaluate_scene(scene,stack->anim,t,&evaluate,&error);
            check(pose!=NULL,"Cannot evaluate animation pose");
            if(!pose) continue;
            double lo[3]={1e20,1e20,1e20},hi[3]={-1e20,-1e20,-1e20};
            for(size_t m=0;m<pose->meshes.count;m++) {
                const ufbx_mesh *pm=pose->meshes.data[m],*sm=start->meshes.data[m];
                for(size_t v=0;v<pm->skinned_position.values.count;v++) {
                    ufbx_vec3 p=pm->skinned_position.values.data[v],q=sm->skinned_position.values.data[v];
                    check(isfinite(p.x)&&isfinite(p.y)&&isfinite(p.z),"Nonfinite skinning");
                    double d=sqrt((p.x-q.x)*(p.x-q.x)+(p.y-q.y)*(p.y-q.y)+(p.z-q.z)*(p.z-q.z));
                    if(d>maxmotion) maxmotion=d;
                    if(sample==8 && d>enddiff) enddiff=d;
                    double vals[3]={p.x,p.y,p.z};
                    for(int k=0;k<3;k++) { if(vals[k]<lo[k]) lo[k]=vals[k]; if(vals[k]>hi[k]) hi[k]=vals[k]; }
                }
            }
            if(lo[1]<minheight) minheight=lo[1];
            for(int k=0;k<3;k++) if(hi[k]-lo[k]>maxextent) maxextent=hi[k]-lo[k];
            ufbx_free_scene(pose);
        }
        check(maxmotion>.001,"Static animation clip");
        check(maxextent<3.5,"Exploding skin or invalid scale");
        check(minheight>-.06,"Excessive ground penetration");
        if(loops[index]) check(enddiff<.004,"Loop endpoint discontinuity");
        printf("CLIP %s duration=%.3f max_motion=%.4f loop_error=%.5f min_height=%.4f max_extent=%.4f\n",
               stack->name.data,seconds[index],maxmotion,enddiff,minheight,maxextent);
        ufbx_free_scene(start);
    }
    ufbx_free_scene(scene);
    printf("RESULT failures=%d\n",failures);
    return failures?1:0;
}
