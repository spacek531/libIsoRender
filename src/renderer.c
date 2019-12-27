#include<stdio.h>
#include<stdlib.h>
#include<math.h>
#include<string.h>
#include<stdbool.h>
#include<assert.h>
#include<errno.h>
#include "renderer.h"
#include "palette.h"
#include "vectormath.h"
#include "model.h"

//3.67 metres per tile
#define SQRT_2 1.4142135623731
#define SQRT1_2 0.707106781
#define SQRT_3 1.73205080757
#define SQRT_6 2.44948974278

matrix_t views[4]={{1,0,0,0,1,0,0,0,1},{0,0,1,0,1,0,-1,0,0},{-1,0,0,0,1,0,0,0,-1},{0,0,-1,0,1,0,1,0,0}};

#define AO_NUM_SAMPLES_U 8
#define AO_NUM_SAMPLES_V 4
#define AA_NUM_SAMPLES_U 2
#define AA_NUM_SAMPLES_V 2

void context_init(context_t* context,light_t* lights,uint32_t num_lights,palette_t palette,float upt)
	{
	context->rt_device=device_init();
	context->lights=lights;
	context->num_lights=num_lights;
	//Dimetric projection
	const matrix_t projection={
		32.0/upt		,0.0	   		,-32.0/upt,
		 -16.0/upt	,-16.0*SQRT_6/upt	,-16.0/upt,
		 16.0*SQRT_3/upt	,-16.0*SQRT_2/upt	,16.0*SQRT_3/upt
		};
	context->projection=projection;
	context->palette=palette;
	}

void context_begin_render(context_t* context)
{
scene_init(&(context->rt_scene),context->rt_device);
}

vertex_t linear_transform(vector3_t vertex,vector3_t normal,void* matptr)
{
transform_t transform=*((transform_t*)matptr);
vertex_t out;
out.vertex=transform_vector(transform,vertex);
out.normal=vector3_normalize(matrix_vector(transform.matrix,normal));
return out;
}

void context_add_model_transformed(context_t* context,mesh_t* mesh,vertex_t (*transform)(vector3_t,vector3_t,void*),void* data)
{
scene_add_model(&(context->rt_scene),mesh,transform,data);
}

void context_add_model(context_t* context,mesh_t* mesh,transform_t transform)
{
scene_add_model(&(context->rt_scene),mesh,&linear_transform,&transform);
}

void context_finalize_render(context_t* context)
{
scene_finalize(&(context->rt_scene));
}

void context_end_render(context_t* context)
{
scene_destroy(&(context->rt_scene));
}

void context_destroy(context_t* context)
{
device_destroy(context->rt_device);
}

//Specular shading code from Blender. Not sure what it does
float spec(float inp, int hard)  
	{
		if (inp>=1.0f) return 1.0f;
		else if (inp<=0.0f) return 0.0f;
        
        float b1=inp*inp;
        	if (b1<0.01f) b1= 0.01f;
        
		if ((hard & 1)==0)  inp= 1.0f;
		if (hard&2)  inp*= b1;
        b1*= b1;
		if (hard&4)  inp*= b1;
        b1*= b1;
		if (hard&8)  inp*= b1;
        b1*= b1;
		if (hard&16) inp*= b1;
        b1*= b1;

		if (b1<0.001f) b1= 0.0f;

		if (hard&32) inp*= b1;
        b1*= b1;
		if (hard&64) inp*=b1;
        b1*= b1;
		if (hard&128) inp*=b1;

		if (b1<0.001f) b1= 0.0f;

        	if (hard & 256)
		{
                b1*= b1;
                inp*=b1;
        	}
        return inp;
	}

float cook_torr_spec(vector3_t n,vector3_t l,vector3_t v,int hard)
	{
        vector3_t h=vector3_normalize(vector3_add(v,l));

        float nh=vector3_dot(n,h);
        	if(nh<0.0f) return 0.0f;
        float nv=vector3_dot(n,v);
        	if (nv<0.0f) nv= 0.0f;

        return spec(nh, hard)/(0.1f+nv);
	}


vector3_t shade_fragment(vector3_t normal,vector3_t view,vector3_t color,vector3_t specular_color,uint32_t specular_hardness,light_t* lights,uint32_t num_lights)
	{
	vector3_t output_color=vector3(0,0,0);
		for(uint32_t i=0;i<num_lights;i++)
		{
			if(lights[i].type==LIGHT_HEMI)
			{
			float diffuse_factor=0.5*lights[i].intensity*(1+vector3_dot(normal,lights[i].direction));
			output_color=vector3_add(vector3_mult(color,diffuse_factor),output_color);
			}
			else if(lights[i].type==LIGHT_DIFFUSE)
			{
			float diffuse_factor=lights[i].intensity*fmax(vector3_dot(normal,lights[i].direction),0.0);
			output_color=vector3_add(vector3_mult(color,diffuse_factor),output_color);
			}
			else
			{
			float diffuse_factor=lights[i].intensity*fmax(vector3_dot(normal,lights[i].direction),0.0);
			//vector3_t reflected_light_direction=vector3_sub(vector3_mult(normal,2.0*vector3_dot(lights[i].direction,normal)),lights[i].direction);
			
			float specular_factor=lights[i].intensity*cook_torr_spec(normal,lights[i].direction,view,specular_hardness);
			output_color=vector3_add(vector3_add(vector3_mult(color,diffuse_factor),vector3_mult(specular_color,specular_factor)),output_color);	
			}	
		}
	return output_color; 
	}

int scene_sample_point(scene_t* scene,vector2_t point,matrix_t camera,light_t* lights,uint32_t num_lights,fragment_t* fragment)
{
ray_hit_t hit;
vector3_t view_vector=matrix_vector(camera,vector3(0,0,-1));
	if(scene_trace_ray(scene,matrix_vector(camera,vector3(point.x,point.y,-512)),vector3_mult(view_vector,-1),&hit))
	{
	
	mesh_t* mesh=scene->meshes[hit.mesh_index];
	face_t* face=mesh->faces+hit.face_index;
	material_t* material=mesh->materials+face->material;

	//Check if this is a mask
		if(material->flags&MATERIAL_IS_MASK)return 0;

	//Compute surface color
	vector3_t color;
		if(material->flags&MATERIAL_HAS_TEXTURE)
		{
		vector2_t tex_coord=vector2_add(vector2_add(vector2_mult(mesh->uvs[face->indices[0]],1.0-hit.u-hit.v),vector2_mult(mesh->uvs[face->indices[1]],hit.u)),vector2_mult(mesh->uvs[face->indices[2]],hit.v));
		color=texture_sample(&(material->texture),tex_coord);
		}
		else color=material->color;
	//Remappable colors should be rendered as grayscale
		if(material->flags&MATERIAL_IS_REMAPPABLE)
		{
		float intensity=fmax(fmax(material->color.x,material->color.y),material->color.z);
		material->color=vector3_from_scalar(intensity);
		}
		

	//Shade fragment
	vector3_t shaded_color=shade_fragment(hit.normal,view_vector,color,material->specular_color,material->specular_hardness,lights,num_lights);
	
	vector3_t normal=hit.normal;
	vector3_t tangent;
		if(fabs(normal.x)>fabs(normal.y))tangent=vector3_mult(vector3(normal.z,0,-normal.x),1.0/sqrt(normal.x*normal.x+normal.z*normal.z)); 
		else tangent=vector3_mult(vector3(0,-normal.z,normal.y),1.0/sqrt(normal.y*normal.y+normal.z*normal.z)); 
	vector3_t bitangent=vector3_cross(normal,tangent);

	uint32_t occluded_samples=0;
		for(int i=0;i<AO_NUM_SAMPLES_U;i++)
		for(int j=0;j<AO_NUM_SAMPLES_V;j++)
		{
		float theta=2*M_PI*((i+(((float)rand())/RAND_MAX))/AO_NUM_SAMPLES_U);
		float phi=asin(1-((j+(((float)rand())/RAND_MAX))/AO_NUM_SAMPLES_V));
	
		vector3_t local_sample_dir=vector3(cos(phi)*sin(theta),cos(phi)*cos(theta),sin(phi));
		vector3_t sample_dir=vector3_add(vector3_mult(normal,local_sample_dir.z),vector3_add(vector3_mult(tangent,local_sample_dir.x),vector3_mult(bitangent,local_sample_dir.y)));
			if(scene_trace_occlusion_ray(scene,hit.position,sample_dir))occluded_samples++;
		}
	float ao_factor=((float)occluded_samples)/(AO_NUM_SAMPLES_U*AO_NUM_SAMPLES_V);
	
	//Write result
	fragment->color=vector3_mult(shaded_color,ao_factor);
	fragment->region=material->region;
	return 1;
	}
return 0;
}
int scene_sample_region(scene_t* scene,vector2_t point,matrix_t camera,uint32_t* region)
{
ray_hit_t hit;
vector3_t view_vector=matrix_vector(camera,vector3(0,0,-1));
	if(scene_trace_ray(scene,matrix_vector(camera,vector3(point.x,point.y,-512)),vector3_mult(view_vector,-1),&hit))
	{
	
	mesh_t* mesh=scene->meshes[hit.mesh_index];
	face_t* face=mesh->faces+hit.face_index;
	material_t* material=mesh->materials+face->material;

		if(!(material->flags&MATERIAL_IS_MASK))
		{
		*region=material->region;
		return 1;
		}
	}
*region=FRAGMENT_UNUSED;
return 0;
}

rect_t rect(int xl,int xu,int yl,int yu)
{
rect_t result={xl,yl,xu,yu};
return result;
}
rect_t rect_enclose_point(rect_t r,float x,float y)
{
return rect((int)fmin(r.x_lower,floor(x)),(int)fmax(r.x_upper,ceil(x)),
	    (int)fmin(r.y_lower,floor(y)),(int)fmax(r.y_upper,ceil(y)));
}

rect_t scene_get_bounds(scene_t* scene,matrix_t camera)
{
vector3_t bounding_points[8]={
vector3(scene->x_min,scene->y_min,scene->z_min),
vector3(scene->x_max,scene->y_min,scene->z_min),
vector3(scene->x_min,scene->y_max,scene->z_min),
vector3(scene->x_max,scene->y_max,scene->z_min),
vector3(scene->x_min,scene->y_min,scene->z_max),
vector3(scene->x_max,scene->y_min,scene->z_max),
vector3(scene->x_min,scene->y_max,scene->z_max),
vector3(scene->x_max,scene->y_max,scene->z_max)};

rect_t bounds=rect((int)floor(bounding_points[0].x),(int)ceil(bounding_points[0].x),(int)floor(bounding_points[0].y),(int)ceil(bounding_points[0].y));
	for(int j=0;j<8;j++)
	{
	vector3_t screen_point=matrix_vector(camera,bounding_points[j]);
	bounds=rect_enclose_point(bounds,screen_point.x,screen_point.y);
	}
return bounds;
}


#define FRAMEBUFFER_INDEX(fbf,x,y) (framebuffer->fragments[(x)+(y)*framebuffer->width])


rect_t framebuffer_get_bounds(framebuffer_t* framebuffer)
{
//printf("%d %d\n",framebuffer->width,framebuffer->height);
//return rect(0,framebuffer->width,0,framebuffer->height);
int found_pixel=0;
rect_t bounds;
	for(uint32_t y=0;y<framebuffer->height;y++)
	for(uint32_t x=0;x<framebuffer->width;x++)
	{
		if(FRAMEBUFFER_INDEX(framebuffer,x,y).region!=FRAGMENT_UNUSED)
		{
			if(found_pixel)bounds=rect_enclose_point(bounds,x,y);
			else
			{
			bounds=rect(x,x+1,y,y+1);
			found_pixel=1;
			}
		}
	}
	//If the image is empty, just set the size as 1 pixel
	if(!found_pixel)return rect(0,0,0,0);
	else return bounds;
}
void image_from_framebuffer(image_t* image,framebuffer_t* framebuffer,palette_t* palette)
{
rect_t bounding_box=framebuffer_get_bounds(framebuffer);
image->width=1+bounding_box.x_upper-bounding_box.x_lower;
image->height=1+bounding_box.y_upper-bounding_box.y_lower;
image->x_offset=bounding_box.x_lower+floor(framebuffer->offset.x);
image->y_offset=bounding_box.y_lower+floor(framebuffer->offset.y)-1;//1 compensates for error not sure why it's needed TODO work out why it's needed
image->pixels=calloc(image->width*image->height,sizeof(uint8_t));

	for(int y=bounding_box.y_lower;y<=bounding_box.y_upper;y++)
	{
	int start=(y&1)?bounding_box.x_upper:bounding_box.x_lower;
	int stop=(y&1)?bounding_box.x_lower:bounding_box.x_upper;
	int step=(y&1)?-1:1;
		for(int x=start;x!=stop;x+=step)
		{
		fragment_t fragment=FRAMEBUFFER_INDEX(framebuffer,x,y);
		fragment.color=vector_from_color(color_from_vector(fragment.color));
			if(fragment.region!=FRAGMENT_UNUSED)
			{
			vector3_t error;
			image->pixels[(x-bounding_box.x_lower)+(y-bounding_box.y_lower)*image->width]=palette_get_nearest(palette,fragment.region&REGION_MASK,fragment.color,&error);
						
			//Distribute error onto neighbouring points
			int points[4][2]={{x+step,y},{x-step,y+1},{x,y+1},{x+step,y+1}};
			float weights[4]={7.0/16.0,3.0/16.0,5.0/16.0,1.0/16.0};
				for(int i=0;i<4;i++)
				if(points[i][0]>=0&&points[i][0]<framebuffer->width-1&&points[i][1]>=0&&points[i][1]<framebuffer->height-1)
				{		
				FRAMEBUFFER_INDEX(framebuffer,points[i][0],points[i][1]).color=vector3_add(vector3_mult(error,0.3*weights[i]),FRAMEBUFFER_INDEX(framebuffer,points[i][0],points[i][1]).color);
				}
			}
		}
	}
free(framebuffer->fragments);
}

void framebuffer_save_bmp(framebuffer_t* framebuffer,char* filename)
{
int padding=(4-(framebuffer->width*3)%4)%4;
int data_size=framebuffer->height*(framebuffer->width*3+padding);

uint8_t bitmap_header[54];
memset(bitmap_header,0,54);

bitmap_header[0]='B';
bitmap_header[1]='M';
*((uint32_t*)(bitmap_header+2))=54+data_size;
*((uint32_t*)(bitmap_header+10))=54;
*((uint32_t*)(bitmap_header+14))=40;
*((uint32_t*)(bitmap_header+18))=framebuffer->width;
*((uint32_t*)(bitmap_header+22))=framebuffer->height;
*((uint16_t*)(bitmap_header+26))=1;
*((uint16_t*)(bitmap_header+28))=24;
*((uint32_t*)(bitmap_header+38))=2834;
*((uint32_t*)(bitmap_header+42))=2834;

FILE* file=fopen(filename,"wb");
	if(file==NULL)
	{
	printf("File open failed %d\n",errno);
	return;
	}

fwrite(bitmap_header,1,54,file);

	for(int32_t y=framebuffer->height-1;y>=0;y--)
	{
		for(uint32_t x=0;x<framebuffer->width;x++)
		{
		color_t color=color_from_vector(framebuffer->fragments[x+y*framebuffer->width].color);
		fputc(color.b,file);
		fputc(color.g,file);
		fputc(color.r,file);
		}
		for(uint32_t k=0;k<padding;k++)fputc(0,file);
	}

fclose(file);
}

void context_render_view_internal(context_t* context,matrix_t view,image_t* image,uint32_t silhouette)
{
matrix_t camera=matrix_mult(context->projection,view);


rect_t bounds=scene_get_bounds(&(context->rt_scene),camera);

framebuffer_t framebuffer;
framebuffer.width=bounds.x_upper-bounds.x_lower+1;
framebuffer.height=bounds.y_upper-bounds.y_lower;
framebuffer.offset=vector2((float)(bounds.x_lower)-0.5,(float)(bounds.y_lower));
framebuffer.fragments=malloc(framebuffer.width*framebuffer.height*sizeof(fragment_t));


//Transform lights for view
light_t transformed_lights[context->num_lights];
matrix_t view_inverse=matrix_inverse(view);
	for(int i=0;i<context->num_lights;i++)
	{
	transformed_lights[i].type=context->lights[i].type;
	transformed_lights[i].direction=matrix_vector(view_inverse,context->lights[i].direction);
	transformed_lights[i].intensity=context->lights[i].intensity;
	}


//Render image
	for(int i=0;i<framebuffer.width*framebuffer.height;i++)
	{
	framebuffer.fragments[i].color=vector3(0.0,0.0,0.0);
	framebuffer.fragments[i].region=FRAGMENT_UNUSED;
	}

matrix_t camera_inverse=matrix_inverse(camera);
	for(int y=0;y<framebuffer.height;y++)
	for(int x=0;x<framebuffer.width;x++)
	{
	vector2_t sample_point=vector2_add(vector2(x,y),framebuffer.offset);
	uint32_t region=FRAGMENT_UNUSED;
		if(scene_sample_region(&(context->rt_scene),sample_point,camera_inverse,&region))
		{
			if(silhouette)
			{
			framebuffer.fragments[x+y*framebuffer.width].color=vector3(1.0,1.0,1.0);
			framebuffer.fragments[x+y*framebuffer.width].region=region;
			continue;
			}

		vector3_t subsample_total=vector3(0.0,0.0,0.0);
		float num_subsamples=0.0;
			for(int i=0;i<AA_NUM_SAMPLES_U;i++)
			for(int j=0;j<AA_NUM_SAMPLES_V;j++)
			{
			fragment_t subsample;
			vector2_t subsample_point=vector2_add(sample_point,vector2((i+(((float)rand())/RAND_MAX))/AA_NUM_SAMPLES_U,(j+(((float)rand())/RAND_MAX))/AA_NUM_SAMPLES_V));
				if(scene_sample_point(&(context->rt_scene),subsample_point,camera_inverse,transformed_lights,context->num_lights,&subsample))
				{
				subsample_total=vector3_add(subsample_total,subsample.color);
				num_subsamples+=1.0;
				}
			}
			if(num_subsamples!=0.0)
			{
			framebuffer.fragments[x+y*framebuffer.width].color=vector3_mult(subsample_total,1.0/num_subsamples);
			framebuffer.fragments[x+y*framebuffer.width].region=region;
			}
			else
			{
			//This is an edge case; but it ensures that the AA never changes the outline of the image, which is important for sprites that tesselate
			scene_sample_point(&(context->rt_scene),sample_point,camera_inverse,transformed_lights,context->num_lights,&(framebuffer.fragments[x+y*framebuffer.width]));
			}
		}
	}
framebuffer_save_bmp(&framebuffer,"test.bmp");
//Convert to indexed color
image_from_framebuffer(image,&framebuffer,&(context->palette));
}

void context_render_view(context_t* context,matrix_t view,image_t* image)
{
context_render_view_internal(context,view,image,0);
}

void context_render_silhouette(context_t* context,matrix_t view,image_t* image)
{
context_render_view_internal(context,view,image,1);
}
