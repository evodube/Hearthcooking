// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include "../sim/Math.h"
#include "Render3D.h"

// The kitchen's 3D: a little scene of its own (not in the world), drawn by Render3D over the paused, blurred game.
// Everything in it is a "prop": a copy of a mesh (ours, or the game's own food), placed in kitchen space (game units,
// z up, the worktop at z = 0, the same as src/sim/Kitchen.h), with how it should look (tinted, browned, cut).
// The menu that pauses the game while you cook lives here too.
namespace Scene
{
	using Id = int;

	struct Look
	{
		float tint[3]{ 1, 1, 1 };
		float lift[4]{};
		float emissive[3]{};
		float clips[6][4]{};  // world planes: n.p > d isn't drawn (n = 0 for none) - a piece cut from the whole
		float flesh[4]{};     // rgb, 1: where it's cut, the inside shows this colour (a solid cut face)
		float shape[4][4]{};  // the whole's rough shape: middle (w: how square its ends), then its axes over its radii
		float uv[4]{ 1, 1, 0, 0 };
		int   cookMode = 0;  // see Render3D::Item
		float cook[12]{};
		float cookAxis[3]{ 1, 0, 0 };
		float cookRef[3]{ 0, 0, -1 };
		ID3D11ShaderResourceView* skin = nullptr;  // in place of its own colour texture (a piece wears its ingredient's)
		bool  shadow = true;
		ID3D11ShaderResourceView* fleshTex = nullptr;  // its inside, where it's cut (see Render3D::Item)
		bool  sear = false;     // cooks darker (meat over a fire), not golden-brown
		bool  unshadowed = false;  // takes no shadow
		bool  occludes = true;     // shades what's under and beside it (a rough ball the size of it)
		float hollow[4]{};         // a hollow it's in or is (a pot): its rim's middle and radius
		float hollowDepth = 0.0f;  // how deep the hollow is (0: none)
		float fx[4]{};             // lumps (unkneaded dough), flour dusted on, crust (blistered, uneven), wet glisten (0..1)
		float spot[4]{};           // where the flour is: middle (world), radius (0: all over)
		float fx2[4]{};            // a liquid's surface rippling (by how hard it boils), fat glinting on it (0..1)
		float alpha = 1.0f;     // below 1: see-through (blended)
		bool  onlyFlat = false; // only its flattest shape: a dish's food without its bowl
		bool  hideFlat = false; // all but its flattest shape: a pot without the food modelled in it
	};

	void Register();  // kDataLoaded: the menu
	void Open();      // shows the menu (pauses the game); the kitchen fades in
	void Close();
	bool IsOpen();

	void  Preload(const std::vector<std::string>& a_models);  // at load: the kitchen's meshes read in ahead
	Id    Add(const std::string& a_model);  // meshes\-relative; hidden until placed; -1 if it couldn't load
	void  Remove(Id a_id);
	void  RemoveAll();
	bool  Valid(Id a_id);
	// A texture swap, as a form's alternate textures give it: the shape named a_shape (or the a_index-th, if no name)
	// wears these instead of its own
	// Leaves out the mesh's parts whose names hold any of these (lower case): drawn and measured without them
	void  HideShapes(Id a_id, std::vector<std::string> a_words);
	// A dish's food texture: the diffuse of its part that isn't the bowl or plate (with the form's swaps); null till loaded
	::ID3D11ShaderResourceView* FoodTexture(Id a_id);
	void  Solid(Id a_id);  // food: drawn whole, whatever its mesh's alpha says (a cabbage's alpha cut it all away)
	void  SwapTextures(Id a_id, const std::string& a_shape, int a_index, const std::string& a_diffuse, const std::string& a_normal);
	void  Place(Id a_id, const hc::Vec3& a_pos, const hc::Quat& a_rot, float a_scale = 1.0f);
	void  Stretch(Id a_id, float a_x, float a_y, float a_z);       // along its own axes
	void  StretchWorld(Id a_id, float a_x, float a_y, float a_z);  // along the kitchen's axes, about its origin (fitting a mesh to a space)
	void  Hide(Id a_id, bool a_hidden);
	Look& LookOf(Id a_id);
	bool  Bounds(Id a_id, hc::Vec3& a_min, hc::Vec3& a_max);
	bool  FlatBounds(Id a_id, hc::Vec3& a_min, hc::Vec3& a_max);  // its flattest shape's (the food in a dish), or the whole's  // in its own space, once the graphics card has said
	ID3D11ShaderResourceView* Skin(Id a_id);                  // its own colour texture
	ID3D11ShaderResourceView* Texture(const char* a_path);    // a texture by path (textures\...)

	// The camera: looking down at the config's tilt toward a_target from a_dist away; it eases there
	void SetCamera(const hc::Vec3& a_target, float a_dist, bool a_snap = false);
	void SetFire(const hc::Vec3& a_pos, float a_heat);  // the fire's light; heat 0 is none
	void SetHaze(const hc::Vec3& a_pos, float a_amount);  // heat shimmer rising from here; 0 is none
	void SetSprites(std::vector<Render3D::Sprite>&& a_sprites);

	// Kitchen space <-> screen pixels
	bool  Project(const hc::Vec3& a_p, float& a_sx, float& a_sy);
	bool  Unproject(float a_sx, float a_sy, double a_planeZ, double& a_x, double& a_y);
	float PixelsPerUnit(const hc::Vec3& a_at);

	void Update(float a_dt);  // every frame, from the menu (main thread), after the cooking has moved things
}
