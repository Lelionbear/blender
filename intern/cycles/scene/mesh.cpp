/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>

#include "bvh/build.h"
#include "bvh/bvh.h"

#include "device/device.h"

#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader_graph.h"

#include "subd/split.h"

#include "util/log.h"
#include "util/set.h"

CCL_NAMESPACE_BEGIN

/* Triangle */

void Mesh::Triangle::bounds_grow(const float3 *verts, BoundBox &bounds) const
{
  bounds.grow(verts[v[0]]);
  bounds.grow(verts[v[1]]);
  bounds.grow(verts[v[2]]);
}

void Mesh::Triangle::motion_verts(const float3 *verts,
                                  const float3 *vert_steps,
                                  const size_t num_verts,
                                  const size_t num_steps,
                                  const float time,
                                  float3 r_verts[3]) const
{
  /* Figure out which steps we need to fetch and their interpolation factor. */
  const size_t max_step = num_steps - 1;
  const size_t step = min((size_t)(time * max_step), max_step - 1);
  const float t = time * max_step - step;
  /* Fetch vertex coordinates. */
  float3 curr_verts[3];
  float3 next_verts[3];
  verts_for_step(verts, vert_steps, num_verts, num_steps, step, curr_verts);
  verts_for_step(verts, vert_steps, num_verts, num_steps, step + 1, next_verts);
  /* Interpolate between steps. */
  r_verts[0] = (1.0f - t) * curr_verts[0] + t * next_verts[0];
  r_verts[1] = (1.0f - t) * curr_verts[1] + t * next_verts[1];
  r_verts[2] = (1.0f - t) * curr_verts[2] + t * next_verts[2];
}

void Mesh::Triangle::verts_for_step(const float3 *verts,
                                    const float3 *vert_steps,
                                    const size_t num_verts,
                                    const size_t num_steps,
                                    size_t step,
                                    float3 r_verts[3]) const
{
  const size_t center_step = ((num_steps - 1) / 2);
  if (step == center_step) {
    /* Center step: regular vertex location. */
    r_verts[0] = verts[v[0]];
    r_verts[1] = verts[v[1]];
    r_verts[2] = verts[v[2]];
  }
  else {
    /* Center step not stored in the attribute array. */
    if (step > center_step) {
      step--;
    }
    const size_t offset = step * num_verts;
    r_verts[0] = vert_steps[offset + v[0]];
    r_verts[1] = vert_steps[offset + v[1]];
    r_verts[2] = vert_steps[offset + v[2]];
  }
}

float3 Mesh::Triangle::compute_normal(const float3 *verts) const
{
  const float3 &v0 = verts[v[0]];
  const float3 &v1 = verts[v[1]];
  const float3 &v2 = verts[v[2]];
  const float3 norm = cross(v1 - v0, v2 - v0);
  const float normlen = len(norm);
  if (normlen == 0.0f) {
    return make_float3(1.0f, 0.0f, 0.0f);
  }
  return norm / normlen;
}

bool Mesh::Triangle::valid(const float3 *verts) const
{
  return isfinite_safe(verts[v[0]]) && isfinite_safe(verts[v[1]]) && isfinite_safe(verts[v[2]]);
}

/* SubdFace */

float3 Mesh::SubdFace::normal(const Mesh *mesh) const
{
  const float3 v0 = mesh->verts[mesh->subd_face_corners[start_corner + 0]];
  const float3 v1 = mesh->verts[mesh->subd_face_corners[start_corner + 1]];
  const float3 v2 = mesh->verts[mesh->subd_face_corners[start_corner + 2]];

  return safe_normalize(cross(v1 - v0, v2 - v0));
}

/* Mesh */

NODE_DEFINE(Mesh)
{
  NodeType *type = NodeType::add("mesh", create, NodeType::NONE, Geometry::get_node_base_type());

  SOCKET_INT_ARRAY(triangles, "Triangles", array<int>());
  SOCKET_POINT_ARRAY(verts, "Vertices", array<float3>());
  SOCKET_INT_ARRAY(shader, "Shader", array<int>());
  SOCKET_BOOLEAN_ARRAY(smooth, "Smooth", array<bool>());

  static NodeEnum subdivision_type_enum;
  subdivision_type_enum.insert("none", SUBDIVISION_NONE);
  subdivision_type_enum.insert("linear", SUBDIVISION_LINEAR);
  subdivision_type_enum.insert("catmull_clark", SUBDIVISION_CATMULL_CLARK);
  SOCKET_ENUM(subdivision_type, "Subdivision Type", subdivision_type_enum, SUBDIVISION_NONE);

  static NodeEnum subdivision_boundary_interpolation_enum;
  subdivision_boundary_interpolation_enum.insert("none", SUBDIVISION_BOUNDARY_NONE);
  subdivision_boundary_interpolation_enum.insert("edge_only", SUBDIVISION_BOUNDARY_EDGE_ONLY);
  subdivision_boundary_interpolation_enum.insert("edge_and_corner",
                                                 SUBDIVISION_BOUNDARY_EDGE_AND_CORNER);
  SOCKET_ENUM(subdivision_boundary_interpolation,
              "Subdivision Boundary Interpolation",
              subdivision_boundary_interpolation_enum,
              SUBDIVISION_BOUNDARY_EDGE_AND_CORNER);

  static NodeEnum subdivision_fvar_interpolation_enum;
  subdivision_fvar_interpolation_enum.insert("none", SUBDIVISION_FVAR_LINEAR_NONE);
  subdivision_fvar_interpolation_enum.insert("corners_only", SUBDIVISION_FVAR_LINEAR_CORNERS_ONLY);
  subdivision_fvar_interpolation_enum.insert("corners_plus1",
                                             SUBDIVISION_FVAR_LINEAR_CORNERS_PLUS1);
  subdivision_fvar_interpolation_enum.insert("corners_plus2",
                                             SUBDIVISION_FVAR_LINEAR_CORNERS_PLUS2);
  subdivision_fvar_interpolation_enum.insert("boundaries", SUBDIVISION_FVAR_LINEAR_BOUNDARIES);
  subdivision_fvar_interpolation_enum.insert("all", SUBDIVISION_FVAR_LINEAR_ALL);
  SOCKET_ENUM(subdivision_fvar_interpolation,
              "Subdivision Face-Varying Interpolation",
              subdivision_fvar_interpolation_enum,
              SUBDIVISION_FVAR_LINEAR_BOUNDARIES);

  SOCKET_INT_ARRAY(subd_vert_creases, "Subdivision Vertex Crease", array<int>());
  SOCKET_FLOAT_ARRAY(
      subd_vert_creases_weight, "Subdivision Vertex Crease Weights", array<float>());
  SOCKET_INT_ARRAY(subd_creases_edge, "Subdivision Crease Edges", array<int>());
  SOCKET_FLOAT_ARRAY(subd_creases_weight, "Subdivision Crease Weights", array<float>());
  SOCKET_INT_ARRAY(subd_face_corners, "Subdivision Face Corners", array<int>());
  SOCKET_INT_ARRAY(subd_start_corner, "Subdivision Face Start Corner", array<int>());
  SOCKET_INT_ARRAY(subd_num_corners, "Subdivision Face Corner Count", array<int>());
  SOCKET_INT_ARRAY(subd_shader, "Subdivision Face Shader", array<int>());
  SOCKET_BOOLEAN_ARRAY(subd_smooth, "Subdivision Face Smooth", array<bool>());
  SOCKET_INT_ARRAY(subd_ptex_offset, "Subdivision Face PTex Offset", array<int>());

  /* Subdivisions parameters */
  SOCKET_FLOAT(subd_dicing_rate, "Subdivision Dicing Rate", 1.0f)
  SOCKET_INT(subd_max_level, "Max Subdivision Level", 1);
  SOCKET_TRANSFORM(subd_objecttoworld, "Subdivision Object Transform", transform_identity());

  return type;
}

bool Mesh::need_tesselation()
{
  return (subdivision_type != SUBDIVISION_NONE) &&
         (verts_is_modified() || subd_dicing_rate_is_modified() ||
          subd_objecttoworld_is_modified() || subd_max_level_is_modified());
}

Mesh::Mesh(const NodeType *node_type, Type geom_type_)
    : Geometry(node_type, geom_type_), subd_attributes(this, ATTR_PRIM_SUBD)
{
  vert_offset = 0;

  face_offset = 0;
  corner_offset = 0;

  num_subd_added_verts = 0;
  num_subd_faces = 0;

  subdivision_type = SUBDIVISION_NONE;
}

Mesh::Mesh() : Mesh(get_node_type(), Geometry::MESH) {}

void Mesh::resize_mesh(const int numverts, const int numtris)
{
  verts.resize(numverts);
  triangles.resize(numtris * 3);
  shader.resize(numtris);
  smooth.resize(numtris);

  attributes.resize();
}

void Mesh::reserve_mesh(const int numverts, const int numtris)
{
  /* reserve space to add verts and triangles later */
  verts.reserve(numverts);
  triangles.reserve(numtris * 3);
  shader.reserve(numtris);
  smooth.reserve(numtris);

  attributes.resize(true);
}

void Mesh::resize_subd_faces(const int numfaces, const int numcorners)
{
  subd_start_corner.resize(numfaces);
  subd_num_corners.resize(numfaces);
  subd_shader.resize(numfaces);
  subd_smooth.resize(numfaces);
  subd_ptex_offset.resize(numfaces);
  subd_face_corners.resize(numcorners);
  num_subd_faces = numfaces;

  subd_attributes.resize();
}

void Mesh::reserve_subd_faces(const int numfaces, const int numcorners)
{
  subd_start_corner.reserve(numfaces);
  subd_num_corners.reserve(numfaces);
  subd_shader.reserve(numfaces);
  subd_smooth.reserve(numfaces);
  subd_ptex_offset.reserve(numfaces);
  subd_face_corners.reserve(numcorners);
  num_subd_faces = numfaces;

  subd_attributes.resize(true);
}

void Mesh::reserve_subd_creases(const size_t num_creases)
{
  subd_creases_edge.reserve(num_creases * 2);
  subd_creases_weight.reserve(num_creases);
}

void Mesh::clear_non_sockets()
{
  Geometry::clear(true);

  num_subd_added_verts = 0;
  num_subd_faces = 0;
}

void Mesh::clear(bool preserve_shaders, bool preserve_voxel_data)
{
  Geometry::clear(preserve_shaders);

  /* clear all verts and triangles */
  verts.clear();
  triangles.clear();
  shader.clear();
  smooth.clear();

  subd_start_corner.clear();
  subd_num_corners.clear();
  subd_shader.clear();
  subd_smooth.clear();
  subd_ptex_offset.clear();
  subd_face_corners.clear();

  subd_creases_edge.clear();
  subd_creases_weight.clear();

  subd_attributes.clear();
  attributes.clear(preserve_voxel_data);

  subdivision_type = SubdivisionType::SUBDIVISION_NONE;

  clear_non_sockets();
}

void Mesh::clear(bool preserve_shaders)
{
  clear(preserve_shaders, false);
}

void Mesh::add_vertex(const float3 P)
{
  verts.push_back_reserved(P);
  tag_verts_modified();
}

void Mesh::add_vertex_slow(const float3 P)
{
  verts.push_back_slow(P);
  tag_verts_modified();
}

void Mesh::add_triangle(const int v0, const int v1, const int v2, const int shader_, bool smooth_)
{
  triangles.push_back_reserved(v0);
  triangles.push_back_reserved(v1);
  triangles.push_back_reserved(v2);
  shader.push_back_reserved(shader_);
  smooth.push_back_reserved(smooth_);

  tag_triangles_modified();
  tag_shader_modified();
  tag_smooth_modified();
}

void Mesh::add_subd_face(const int *corners,
                         const int num_corners,
                         const int shader_,
                         bool smooth_)
{
  const int start_corner = subd_face_corners.size();

  for (int i = 0; i < num_corners; i++) {
    subd_face_corners.push_back_reserved(corners[i]);
  }

  int ptex_offset = 0;
  // cannot use get_num_subd_faces here as it holds the total number of subd_faces, but we do not
  // have the total amount of data yet
  if (subd_shader.size()) {
    const SubdFace s = get_subd_face(subd_shader.size() - 1);
    ptex_offset = s.ptex_offset + s.num_ptex_faces();
  }

  subd_start_corner.push_back_reserved(start_corner);
  subd_num_corners.push_back_reserved(num_corners);
  subd_shader.push_back_reserved(shader_);
  subd_smooth.push_back_reserved(smooth_);
  subd_ptex_offset.push_back_reserved(ptex_offset);

  tag_subd_face_corners_modified();
  tag_subd_start_corner_modified();
  tag_subd_num_corners_modified();
  tag_subd_shader_modified();
  tag_subd_smooth_modified();
  tag_subd_ptex_offset_modified();
}

Mesh::SubdFace Mesh::get_subd_face(const size_t index) const
{
  Mesh::SubdFace s;
  s.shader = subd_shader[index];
  s.num_corners = subd_num_corners[index];
  s.smooth = subd_smooth[index];
  s.ptex_offset = subd_ptex_offset[index];
  s.start_corner = subd_start_corner[index];
  return s;
}

void Mesh::add_edge_crease(const int v0, const int v1, const float weight)
{
  subd_creases_edge.push_back_slow(v0);
  subd_creases_edge.push_back_slow(v1);
  subd_creases_weight.push_back_slow(weight);

  tag_subd_creases_edge_modified();
  tag_subd_creases_edge_modified();
  tag_subd_creases_weight_modified();
}

void Mesh::add_vertex_crease(const int v, const float weight)
{
  subd_vert_creases.push_back_slow(v);
  subd_vert_creases_weight.push_back_slow(weight);

  tag_subd_vert_creases_modified();
  tag_subd_vert_creases_weight_modified();
}

void Mesh::copy_center_to_motion_step(const int motion_step)
{
  Attribute *attr_mP = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);

  if (attr_mP) {
    Attribute *attr_mN = attributes.find(ATTR_STD_MOTION_VERTEX_NORMAL);
    Attribute *attr_N = attributes.find(ATTR_STD_VERTEX_NORMAL);
    float3 *P = verts.data();
    float3 *N = (attr_N) ? attr_N->data_float3() : nullptr;
    const size_t numverts = verts.size();

    std::copy_n(P, numverts, attr_mP->data_float3() + motion_step * numverts);
    if (attr_mN) {
      std::copy_n(N, numverts, attr_mN->data_float3() + motion_step * numverts);
    }
  }
}

void Mesh::get_uv_tiles(ustring map, unordered_set<int> &tiles)
{
  Attribute *attr;
  Attribute *subd_attr;

  if (map.empty()) {
    attr = attributes.find(ATTR_STD_UV);
    subd_attr = subd_attributes.find(ATTR_STD_UV);
  }
  else {
    attr = attributes.find(map);
    subd_attr = subd_attributes.find(map);
  }

  if (attr) {
    attr->get_uv_tiles(this, ATTR_PRIM_GEOMETRY, tiles);
  }
  if (subd_attr) {
    subd_attr->get_uv_tiles(this, ATTR_PRIM_SUBD, tiles);
  }
}

void Mesh::compute_bounds()
{
  BoundBox bnds = BoundBox::empty;
  const size_t verts_size = verts.size();

  if (verts_size > 0) {
    for (size_t i = 0; i < verts_size; i++) {
      bnds.grow(verts[i]);
    }

    Attribute *attr = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (use_motion_blur && attr) {
      const size_t steps_size = verts.size() * (motion_steps - 1);
      float3 *vert_steps = attr->data_float3();

      for (size_t i = 0; i < steps_size; i++) {
        bnds.grow(vert_steps[i]);
      }
    }

    if (!bnds.valid()) {
      bnds = BoundBox::empty;

      /* skip nan or inf coordinates */
      for (size_t i = 0; i < verts_size; i++) {
        bnds.grow_safe(verts[i]);
      }

      if (use_motion_blur && attr) {
        const size_t steps_size = verts.size() * (motion_steps - 1);
        float3 *vert_steps = attr->data_float3();

        for (size_t i = 0; i < steps_size; i++) {
          bnds.grow_safe(vert_steps[i]);
        }
      }
    }
  }

  if (!bnds.valid()) {
    /* empty mesh */
    bnds.grow(zero_float3());
  }

  bounds = bnds;
}

void Mesh::apply_transform(const Transform &tfm, const bool apply_to_motion)
{
  transform_normal = transform_transposed_inverse(tfm);

  /* apply to mesh vertices */
  for (size_t i = 0; i < verts.size(); i++) {
    verts[i] = transform_point(&tfm, verts[i]);
  }

  tag_verts_modified();

  if (apply_to_motion) {
    Attribute *attr = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);

    if (attr) {
      const size_t steps_size = verts.size() * (motion_steps - 1);
      float3 *vert_steps = attr->data_float3();

      for (size_t i = 0; i < steps_size; i++) {
        vert_steps[i] = transform_point(&tfm, vert_steps[i]);
      }
    }

    Attribute *attr_N = attributes.find(ATTR_STD_MOTION_VERTEX_NORMAL);

    if (attr_N) {
      const Transform ntfm = transform_normal;
      const size_t steps_size = verts.size() * (motion_steps - 1);
      float3 *normal_steps = attr_N->data_float3();

      for (size_t i = 0; i < steps_size; i++) {
        normal_steps[i] = normalize(transform_direction(&ntfm, normal_steps[i]));
      }
    }
  }
}

void Mesh::add_vertex_normals()
{
  const bool flip = transform_negative_scaled;
  const size_t verts_size = verts.size();
  const size_t triangles_size = num_triangles();

  /* static vertex normals */
  if (!attributes.find(ATTR_STD_VERTEX_NORMAL) && triangles_size) {
    /* get attributes */
    Attribute *attr_vN = attributes.add(ATTR_STD_VERTEX_NORMAL);

    float3 *verts_ptr = verts.data();
    float3 *vN = attr_vN->data_float3();

    /* compute vertex normals */
    std::fill_n(vN, verts.size(), zero_float3());

    for (size_t i = 0; i < triangles_size; i++) {
      const float3 fN = get_triangle(i).compute_normal(verts_ptr);
      for (size_t j = 0; j < 3; j++) {
        vN[get_triangle(i).v[j]] += fN;
      }
    }

    if (flip) {
      for (size_t i = 0; i < verts_size; i++) {
        vN[i] = -normalize(vN[i]);
      }
    }
    else {
      for (size_t i = 0; i < verts_size; i++) {
        vN[i] = normalize(vN[i]);
      }
    }
  }

  /* motion vertex normals */
  Attribute *attr_mP = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
  Attribute *attr_mN = attributes.find(ATTR_STD_MOTION_VERTEX_NORMAL);

  if (has_motion_blur() && attr_mP && !attr_mN && triangles_size) {
    /* create attribute */
    attr_mN = attributes.add(ATTR_STD_MOTION_VERTEX_NORMAL);

    for (int step = 0; step < motion_steps - 1; step++) {
      float3 *mP = attr_mP->data_float3() + step * verts.size();
      float3 *mN = attr_mN->data_float3() + step * verts.size();

      /* compute */
      std::fill_n(mN, verts.size(), zero_float3());

      for (size_t i = 0; i < triangles_size; i++) {
        const Triangle tri = get_triangle(i);
        const float3 fN = tri.compute_normal(mP);
        for (size_t j = 0; j < 3; j++) {
          mN[tri.v[j]] += fN;
        }
      }

      if (flip) {
        for (size_t i = 0; i < verts_size; i++) {
          mN[i] = -normalize(mN[i]);
        }
      }
      else {
        for (size_t i = 0; i < verts_size; i++) {
          mN[i] = normalize(mN[i]);
        }
      }
    }
  }

  /* subd vertex normals */
  if (!subd_attributes.find(ATTR_STD_VERTEX_NORMAL) && get_num_subd_faces()) {
    /* get attributes */
    Attribute *attr_vN = subd_attributes.add(ATTR_STD_VERTEX_NORMAL);
    float3 *vN = attr_vN->data_float3();

    /* compute vertex normals */
    std::fill_n(vN, verts.size(), zero_float3());

    for (size_t i = 0; i < get_num_subd_faces(); i++) {
      const SubdFace face = get_subd_face(i);
      const float3 fN = face.normal(this);

      for (size_t j = 0; j < face.num_corners; j++) {
        const size_t corner = subd_face_corners[face.start_corner + j];
        vN[corner] += fN;
      }
    }

    if (flip) {
      for (size_t i = 0; i < verts_size; i++) {
        vN[i] = -normalize(vN[i]);
      }
    }
    else {
      for (size_t i = 0; i < verts_size; i++) {
        vN[i] = normalize(vN[i]);
      }
    }
  }
}

void Mesh::add_undisplaced()
{
  AttributeSet &attrs = (subdivision_type == SUBDIVISION_NONE) ? attributes : subd_attributes;

  /* don't compute if already there */
  if (attrs.find(ATTR_STD_POSITION_UNDISPLACED)) {
    return;
  }

  /* get attribute */
  Attribute *attr = attrs.add(ATTR_STD_POSITION_UNDISPLACED);

  float3 *data = attr->data_float3();

  /* copy verts */
  size_t size = attr->buffer_size(this, ATTR_PRIM_GEOMETRY) / sizeof(float3);

  if (size) {
    std::copy_n(verts.data(), size, data);
  }
}

void Mesh::pack_shaders(Scene *scene, uint *tri_shader)
{
  uint shader_id = 0;
  uint last_shader = -1;
  bool last_smooth = false;

  const size_t triangles_size = num_triangles();
  const int *shader_ptr = shader.data();
  const bool *smooth_ptr = smooth.data();

  for (size_t i = 0; i < triangles_size; i++) {
    const int new_shader = shader_ptr ? shader_ptr[i] : INT_MAX;
    const bool new_smooth = smooth_ptr ? smooth_ptr[i] : false;

    if (new_shader != last_shader || last_smooth != new_smooth) {
      last_shader = new_shader;
      last_smooth = new_smooth;
      Shader *shader = (last_shader < used_shaders.size()) ?
                           static_cast<Shader *>(used_shaders[last_shader]) :
                           scene->default_surface;
      shader_id = scene->shader_manager->get_shader_id(shader, last_smooth);
    }

    tri_shader[i] = shader_id;
  }
}

void Mesh::pack_normals(packed_float3 *vnormal)
{
  Attribute *attr_vN = attributes.find(ATTR_STD_VERTEX_NORMAL);
  if (attr_vN == nullptr) {
    /* Happens on objects with just hair. */
    return;
  }

  const bool do_transform = transform_applied;
  const Transform ntfm = transform_normal;

  float3 *vN = attr_vN->data_float3();
  const size_t verts_size = verts.size();

  if (do_transform) {
    for (size_t i = 0; i < verts_size; i++) {
      vnormal[i] = safe_normalize(transform_direction(&ntfm, vN[i]));
    }
  }
  else {
    for (size_t i = 0; i < verts_size; i++) {
      vnormal[i] = vN[i];
    }
  }
}

void Mesh::pack_verts(packed_float3 *tri_verts, packed_uint3 *tri_vindex)
{
  const size_t verts_size = verts.size();
  const size_t triangles_size = num_triangles();
  const int *p_tris = triangles.data();
  int off = 0;
  for (size_t i = 0; i < verts_size; i++) {
    tri_verts[i] = verts[i];
  }
  for (size_t i = 0; i < triangles_size; i++) {
    tri_vindex[i] = make_packed_uint3(p_tris[off + 0] + vert_offset,
                                      p_tris[off + 1] + vert_offset,
                                      p_tris[off + 2] + vert_offset);
    off += 3;
  }
}

bool Mesh::has_motion_blur() const
{
  return use_motion_blur && (attributes.find(ATTR_STD_MOTION_VERTEX_POSITION) ||
                             (get_subdivision_type() != Mesh::SUBDIVISION_NONE &&
                              subd_attributes.find(ATTR_STD_MOTION_VERTEX_POSITION)));
}

PrimitiveType Mesh::primitive_type() const
{
  return has_motion_blur() ? PRIMITIVE_MOTION_TRIANGLE : PRIMITIVE_TRIANGLE;
}

CCL_NAMESPACE_END
