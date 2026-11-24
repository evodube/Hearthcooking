// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Scene.h"

#include "Config.h"
#include "Cook.h"
#include "Overlay.h"

namespace Scene
{
	namespace
	{
		constexpr std::string_view kMenuName = "HearthcookingMenu";

		struct Prop
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::string                   model;
			Look                          look;
			float                         scale3[3]{ 1, 1, 1 };
			float                         world3[3]{ 1, 1, 1 };
			bool                          used = false, placed = false, hidden = false;
			bool                          measured = false, asked = false;
			int                           tries = 0;  // frames spent waiting for its measurement
			hc::Vec3                      bmin, bmax;
			int                           flatIndex = -1;  // its flattest shape, when it has more than one
			hc::Vec3                      fmin, fmax;
			ID3D11ShaderResourceView*     skin = nullptr;
			struct Swap
			{
				std::string shape, diffuse, normal;
				int         index = -1;
			};
			std::vector<Swap> swaps;
			std::vector<std::string> hideShapes;
		};

		bool LeftOut(const Prop& a_p, RE::BSGeometry* a_geo)
		{
			if (a_p.hideShapes.empty() || !a_geo || !a_geo->name.c_str()) {
				return false;
			}
			std::string n = a_geo->name.c_str();
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const auto& w : a_p.hideShapes) {
				if (n.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		std::vector<Prop>             g_props;
		RE::NiPointer<RE::NiNode>     g_root;
		std::unordered_map<std::string, RE::NiPointer<RE::NiTexture>> g_texCache;
		std::vector<Render3D::Sprite> g_sprites;
		bool                          g_open = false, g_shown = false;
		std::chrono::steady_clock::time_point g_shownAt{};

		// the camera: where it looks, how far, eased toward what's asked
		hc::Vec3 g_camAt{ 0, -1, 0 }, g_camWant{ 0, -1, 0 };
		float    g_camDist = 90.0f, g_camDistWant = 90.0f;
		float    g_vp[16]{};
		float    g_eye[3]{}, g_right[3]{ 1, 0, 0 }, g_up[3]{ 0, 0, 1 }, g_fwd[3]{ 0, 1, 0 };
		float    g_screenW = 1920.0f, g_screenH = 1080.0f;
		float    g_th = 0.34f;  // tan(half the vertical field of view)
		bool     g_placed = false;

		hc::Vec3 g_firePos{ 18, 3, 3 };
		hc::Vec3 g_hazePos{};
		float    g_haze = 0.0f;
		float    g_fireHeat = 0.0f;

		// the game's own sounds (the fire, the bard, the wind) keep playing while it's paused: hush them in the kitchen
		std::vector<std::pair<RE::BGSSoundCategory*, float>> g_hushed;
		void HushGame(bool a_hush)
		{
			if (!a_hush) {
				for (auto& [cat, vol] : g_hushed) {
					cat->SetCategoryVolume(vol);
				}
				g_hushed.clear();
				return;
			}
			if (!g_hushed.empty()) {
				return;
			}
			if (auto* dh = RE::TESDataHandler::GetSingleton()) {
				for (auto* cat : dh->GetFormArray<RE::BGSSoundCategory>()) {
					if (cat && !cat->parentCategory) {
						g_hushed.emplace_back(cat, cat->GetCategoryVolume());
						cat->SetCategoryVolume(0.0f);
					}
				}
			}
		}

		RE::NiPointer<RE::NiAVObject> Instance(const std::string& a_path)
		{
			RE::NiPointer<RE::NiNode>         node;
			RE::BSModelDB::DBTraits::ArgsType args{};
			const auto                        err = RE::BSModelDB::Demand(a_path.c_str(), node, args);
			if (!node) {
				spdlog::warn("Could not load mesh {} (error {})", a_path, static_cast<int>(err));
				return nullptr;
			}
			// each prop needs its own copy: the database hands out one shared node per file
			RE::NiPointer<RE::NiAVObject> copy(static_cast<RE::NiAVObject*>(node->Clone()));
			// the game's meshes carry physics bodies; ours are moved by hand
			RE::BSVisit::TraverseScenegraphObjects(copy.get(), [](RE::NiAVObject* a_obj) {
				if (a_obj) {
					a_obj->collisionObject.reset();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return copy;
		}

		RE::NiMatrix3 FromQuat(const hc::Quat& q)
		{
			RE::NiMatrix3 m;
			const double  w = q.w, x = q.x, y = q.y, z = q.z;
			m.entry[0][0] = static_cast<float>(1 - 2 * (y * y + z * z));
			m.entry[0][1] = static_cast<float>(2 * (x * y - w * z));
			m.entry[0][2] = static_cast<float>(2 * (x * z + w * y));
			m.entry[1][0] = static_cast<float>(2 * (x * y + w * z));
			m.entry[1][1] = static_cast<float>(1 - 2 * (x * x + z * z));
			m.entry[1][2] = static_cast<float>(2 * (y * z - w * x));
			m.entry[2][0] = static_cast<float>(2 * (x * z - w * y));
			m.entry[2][1] = static_cast<float>(2 * (y * z + w * x));
			m.entry[2][2] = static_cast<float>(1 - 2 * (x * x + y * y));
			return m;
		}

		ID3D11ShaderResourceView* SrvOf(RE::NiSourceTexture* a_tex)
		{
			// the renderer's texture record is laid out as NiTexture::RendererData (texture, -, view)
			auto* rt = a_tex ? reinterpret_cast<RE::NiTexture::RendererData*>(a_tex->rendererTexture) : nullptr;
			return rt ? reinterpret_cast<::ID3D11ShaderResourceView*>(rt->resourceView) : nullptr;
		}
		ID3D11ShaderResourceView* SrvOfPath(const char* a_path)
		{
			if (!a_path || !*a_path) {
				return nullptr;
			}
			auto it = g_texCache.find(a_path);
			if (it == g_texCache.end()) {
				RE::NiPointer<RE::NiTexture> tex;
				RE::BSShaderManager::GetTexture(a_path, true, tex, false);
				it = g_texCache.emplace(a_path, tex).first;
			}
			auto* src = it->second ? netimmerse_cast<RE::NiSourceTexture*>(it->second.get()) : nullptr;
			return SrvOf(src);
		}

		bool Visible(RE::NiAVObject* a_obj, RE::NiAVObject* a_top)
		{
			for (auto* o = a_obj; o; o = o->parent) {
				if (o->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
					return false;
				}
				if (o == a_top) {
					break;
				}
			}
			return true;
		}

		// The geometry's place relative to its prop's root (to measure the prop in its own space)
		void Relative(const RE::NiTransform& a_root, const RE::NiTransform& a_geo, float (&a_rot)[9], float (&a_pos)[3], float& a_scale)
		{
			const RE::NiMatrix3 rt = a_root.rotate.Transpose();
			const RE::NiMatrix3 r = rt * a_geo.rotate;
			const float         rs = a_root.scale != 0.0f ? a_root.scale : 1.0f;
			const RE::NiPoint3  d = a_geo.translate - a_root.translate;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					a_rot[i * 3 + j] = r.entry[i][j];
				}
				a_pos[i] = (rt.entry[i][0] * d.x + rt.entry[i][1] * d.y + rt.entry[i][2] * d.z) / rs;
			}
			a_scale = a_geo.scale / rs;
		}

		// Collects a prop's measured geometries into its own-space bounds, once every one is in
		// A shape's buffers: its own, or for a skinned one (some of the game's meat) its skin partition's - drawn as
		// it was modelled, unposed
		struct Buffers
		{
			::ID3D11Buffer* vb = nullptr;
			::ID3D11Buffer* ib = nullptr;
			std::uint64_t   desc = 0;
			std::uint32_t   indexCount = 0, vertexCount = 0;
		};
		bool BuffersOf(RE::BSGeometry* a_geo, Buffers& a_out)
		{
			auto* tri = a_geo ? a_geo->AsTriShape() : nullptr;
			if (!tri) {
				return false;
			}
			auto& gd = a_geo->GetGeometryRuntimeData();
			if (auto* rd = gd.rendererData; rd && rd->vertexBuffer && rd->indexBuffer) {
				a_out.vb = reinterpret_cast<::ID3D11Buffer*>(rd->vertexBuffer);
				a_out.ib = reinterpret_cast<::ID3D11Buffer*>(rd->indexBuffer);
				std::memcpy(&a_out.desc, &rd->vertexDesc, sizeof(a_out.desc));
				a_out.indexCount = static_cast<std::uint32_t>(tri->GetTrishapeRuntimeData().triangleCount) * 3;
				a_out.vertexCount = tri->GetTrishapeRuntimeData().vertexCount;
				return true;
			}
			auto* skin = gd.skinInstance.get();
			auto* part = skin ? skin->skinPartition.get() : nullptr;
			if (!part || part->numPartitions == 0) {
				return false;
			}
			const auto& p0 = part->partitions[0];
			auto*       bd = p0.buffData;
			if (!bd || !bd->vertexBuffer || !bd->indexBuffer) {
				return false;
			}
			a_out.vb = reinterpret_cast<::ID3D11Buffer*>(bd->vertexBuffer);
			a_out.ib = reinterpret_cast<::ID3D11Buffer*>(bd->indexBuffer);
			std::memcpy(&a_out.desc, &bd->vertexDesc, sizeof(a_out.desc));
			a_out.indexCount = static_cast<std::uint32_t>(p0.triangles) * 3;
			a_out.vertexCount = part->vertexCount;
			return true;
		}

		void TryBounds(Prop& a_p)
		{
			if (a_p.measured || !a_p.node) {
				return;
			}
			hc::Vec3 lo{ 1e9, 1e9, 1e9 }, hi{ -1e9, -1e9, -1e9 };
			bool     all = true, any = false;
			int      shapes = 0, noData = 0, index = -1;
			std::string kinds;
			struct ShapeBox
			{
				int      index;
				hc::Vec3 lo, hi;
			};
			std::vector<ShapeBox> boxes;
			RE::BSVisit::TraverseScenegraphGeometries(a_p.node.get(), [&](RE::BSGeometry* a_geo) {
				++index;
				auto* tri = a_geo ? a_geo->AsTriShape() : nullptr;
				auto* rd = a_geo ? a_geo->GetGeometryRuntimeData().rendererData : nullptr;
				if (LeftOut(a_p, a_geo)) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				++shapes;
				if (a_p.tries == 0 && a_geo && a_geo->GetRTTI()) {
					kinds += fmt::format("{}'{}' ", a_geo->GetRTTI()->name, a_geo->name.c_str());
				}
				Buffers bufs;
				if (!BuffersOf(a_geo, bufs)) {
					++noData;
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				(void)rd;
				(void)tri;
				auto*         vb = bufs.vb;
				std::uint64_t desc = bufs.desc;
				const auto id = reinterpret_cast<std::uint64_t>(vb) ^ (static_cast<std::uint64_t>(bufs.vertexCount) << 48) ^ (desc * 0x9E3779B97F4A7C15ull);
				if (!a_p.asked) {
					Render3D::RequestMeasure(id, vb, desc, bufs.vertexCount);
				}
				float mn[3], mx[3];
				if (!Render3D::MeasureResult(id, mn, mx)) {
					all = false;
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				float rot[9], pos[3], sc;
				Relative(a_p.node->world, a_geo->world, rot, pos, sc);
				ShapeBox sb{ index, { 1e9, 1e9, 1e9 }, { -1e9, -1e9, -1e9 } };
				for (int c = 0; c < 8; ++c) {
					const float v[3] = { (c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2] };
					double      w[3];
					for (int i = 0; i < 3; ++i) {
						w[i] = (rot[i * 3] * v[0] + rot[i * 3 + 1] * v[1] + rot[i * 3 + 2] * v[2]) * sc + pos[i];
					}
					lo = { (std::min)(lo.x, w[0]), (std::min)(lo.y, w[1]), (std::min)(lo.z, w[2]) };
					hi = { (std::max)(hi.x, w[0]), (std::max)(hi.y, w[1]), (std::max)(hi.z, w[2]) };
					sb.lo = { (std::min)(sb.lo.x, w[0]), (std::min)(sb.lo.y, w[1]), (std::min)(sb.lo.z, w[2]) };
					sb.hi = { (std::max)(sb.hi.x, w[0]), (std::max)(sb.hi.y, w[1]), (std::max)(sb.hi.z, w[2]) };
				}
				boxes.push_back(sb);
				any = true;
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			a_p.asked = true;
			if (a_p.tries == 0) {
				spdlog::info("Mesh {}: {} shapes ({} without vertex data): {}", a_p.model, shapes, noData, kinds);
			}
			++a_p.tries;
			if (all && any) {
				a_p.measured = true;
				a_p.bmin = lo;
				a_p.bmax = hi;
				// the flattest of its shapes (its height over its width): in a dish, the food lying in the bowl
				a_p.flatIndex = -1;
				a_p.fmin = lo;
				a_p.fmax = hi;
				if (boxes.size() > 1) {
					double best = 1e9;
					for (const auto& b : boxes) {
						const double wide = (std::max)(b.hi.x - b.lo.x, b.hi.y - b.lo.y);
						const double flat = wide > 1e-3 ? (b.hi.z - b.lo.z) / wide : 1e9;
						if (flat < best) {
							best = flat;
							a_p.flatIndex = b.index;
							a_p.fmin = b.lo;
							a_p.fmax = b.hi;
						}
					}
				}
				spdlog::info("Measured {}: ({:.1f}, {:.1f}, {:.1f}) .. ({:.1f}, {:.1f}, {:.1f})", a_p.model, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
			} else if (a_p.tries > 45) {
				// the graphics card never said: go by the mesh's bounding sphere, so it shows at all
				const auto&  wb = a_p.node->worldBound;
				const auto&  w = a_p.node->world;
				const float  sc = w.scale != 0.0f ? w.scale : 1.0f;
				const auto   d = wb.center - w.translate;
				const auto   rt = w.rotate.Transpose();
				const double cx = (rt.entry[0][0] * d.x + rt.entry[0][1] * d.y + rt.entry[0][2] * d.z) / sc;
				const double cy = (rt.entry[1][0] * d.x + rt.entry[1][1] * d.y + rt.entry[1][2] * d.z) / sc;
				const double cz = (rt.entry[2][0] * d.x + rt.entry[2][1] * d.y + rt.entry[2][2] * d.z) / sc;
				const double r = wb.radius > 0.0f ? wb.radius / sc * 0.75 : 5.0;
				a_p.measured = true;
				a_p.bmin = { cx - r, cy - r, cz - r * 0.8 };
				a_p.bmax = { cx + r, cy + r, cz + r * 0.8 };
				spdlog::warn("Measured {} by its bounding sphere (radius {:.1f}): {} of {} shapes had no vertex data to read", a_p.model, r, noData, shapes);
			}
		}

		void Camera(float a_dt)
		{
			Overlay::ScreenSize(g_screenW, g_screenH);
			const float k = g_placed ? (std::min)(1.0f, a_dt * 3.0f) : 1.0f;
			g_camAt = g_camAt + (g_camWant - g_camAt) * k;
			g_camDist += (g_camDistWant - g_camDist) * k;
			const auto& cfg = Config::Get();
			const float aspect = g_screenW / (std::max)(1.0f, g_screenH);
			const float vfov = 38.0f * 0.0174533f;
			g_th = std::tan(vfov * 0.5f);
			// narrower screens than 16:9 step back, so the kitchen's width still fits
			const float wide = (std::max)(1.0f, (16.0f / 9.0f) / (std::max)(0.5f, aspect));
			const float dist = g_camDist / std::clamp(cfg.zoom, 0.5f, 2.0f) * wide;
			const float tilt = std::clamp(cfg.tiltDeg, 25.0f, 85.0f) * 0.0174533f;
			const float at[3] = { static_cast<float>(g_camAt.x), static_cast<float>(g_camAt.y), static_cast<float>(g_camAt.z) };
			const float eye[3] = { at[0], at[1] - std::cos(tilt) * dist, at[2] + std::sin(tilt) * dist };
			float       f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
			const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
			for (auto& v : f) {
				v /= fl;
			}
			const float up[3] = { 0, 0, 1 };
			float       r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
			const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			for (auto& v : r) {
				v /= rl;
			}
			const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
			auto        dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			const float view[16] = { r[0], r[1], r[2], -dot(r, eye), u[0], u[1], u[2], -dot(u, eye), -f[0], -f[1], -f[2], dot(f, eye), 0, 0, 0, 1 };
			const float n = 1.0f, fa = 600.0f;
			const float proj[16] = { 1.0f / (g_th * aspect), 0, 0, 0, 0, 1.0f / g_th, 0, 0, 0, 0, fa / (n - fa), n * fa / (n - fa), 0, 0, -1, 0 };
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					float s = 0;
					for (int q = 0; q < 4; ++q) {
						s += proj[i * 4 + q] * view[q * 4 + j];
					}
					g_vp[i * 4 + j] = s;
				}
			}
			std::memcpy(g_eye, eye, sizeof(eye));
			std::memcpy(g_right, r, sizeof(r));
			std::memcpy(g_up, u, sizeof(u));
			std::memcpy(g_fwd, f, sizeof(f));
			g_placed = true;
		}

		void SafeMenuDisplay()
		{
			__try {
				Overlay::OnMenuDisplay();
			} __except (Overlay::SehFilter(GetExceptionInformation(), "MenuDisplay")) {
			}
		}

		// ---- the menu: pauses the game, and is where the kitchen runs each frame
		class KitchenMenu : public RE::IMenu
		{
		public:
			static RE::IMenu* Create()
			{
				using Flag = RE::UI_MENU_FLAGS;
				auto* m = new KitchenMenu();
				// (the world behind frozen, as the Tab menu does: the game stops drawing the cell while the kitchen covers it)
				m->menuFlags.set(Flag::kPausesGame, Flag::kDisablePauseMenu, Flag::kUsesMenuContext, Flag::kCustomRendering, Flag::kRequiresUpdate,
					Flag::kFreezeFrameBackground);
				m->depthPriority = 12;  // over the crafting menu we sit on
				return m;
			}

			RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
			{
				switch (a_message.type.get()) {
				case RE::UI_MESSAGE_TYPE::kShow:
					g_shown = true;
					g_shownAt = std::chrono::steady_clock::now();
					g_placed = false;
					HushGame(true);
					if (auto* subs = RE::SubtitleManager::GetSingleton()) {
						subs->KillSubtitles();
					}
					break;
				case RE::UI_MESSAGE_TYPE::kHide:
				case RE::UI_MESSAGE_TYPE::kForceHide:
					if (g_shown) {
						g_shown = false;
						HushGame(false);
						Render3D::Clear();
					}
					break;
				default:
					break;
				}
				return RE::IMenu::ProcessMessage(a_message);
			}

			void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override
			{
				// menus keep advancing while the game is paused: the kitchen runs here, before the frame draws
				__try {
					Cook::Tick(a_interval);
					Scene::Update(a_interval);
				} __except (Overlay::SehFilter(GetExceptionInformation(), "Tick")) {
					g_failed = true;
				}
				if (g_failed) {
					g_failed = false;
					Cook::Abort();
				}
				RE::IMenu::AdvanceMovie(a_interval, a_currentTime);
			}

			void PostDisplay() override
			{
				RE::IMenu::PostDisplay();
				SafeMenuDisplay();  // the kitchen drawn here, so the game's cursor (a menu above ours) draws over it
			}

		private:
			static inline bool g_failed = false;
		};
	}

	void Register()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->Register(kMenuName, KitchenMenu::Create);
			spdlog::info("Menu registered");
		}
	}

	void Open()
	{
		if (g_open) {
			return;
		}
		g_open = true;
		if (!g_root) {
			g_root = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(64));
		}
		Overlay::SetOpen(true);
		if (auto* q = RE::UIMessageQueue::GetSingleton()) {
			q->AddMessage(kMenuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		}
	}

	void Close()
	{
		if (!g_open) {
			return;
		}
		g_open = false;
		Overlay::SetOpen(false);
		RemoveAll();
		g_sprites.clear();
		Render3D::Clear();
		if (auto* q = RE::UIMessageQueue::GetSingleton()) {
			q->AddMessage(kMenuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		}
	}

	bool IsOpen() { return g_open; }

	void Preload(const std::vector<std::string>& a_models)
	{
		// loaded once now (the game's model cache keeps them while they're held), so opening the kitchen only copies them
		static std::vector<RE::NiPointer<RE::NiNode>> s_held;
		for (const auto& m : a_models) {
			RE::NiPointer<RE::NiNode>         node;
			RE::BSModelDB::DBTraits::ArgsType args{};
			RE::BSModelDB::Demand(m.c_str(), node, args);
			if (node) {
				s_held.push_back(node);
			}
		}
	}

	Id Add(const std::string& a_model)
	{
		if (!g_root) {
			g_root = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(64));
		}
		auto node = Instance(a_model);
		if (!node) {
			return -1;
		}
		int id = -1;
		for (int i = 0; i < static_cast<int>(g_props.size()); ++i) {
			if (!g_props[i].used) {
				id = i;
				break;
			}
		}
		if (id < 0) {
			id = static_cast<int>(g_props.size());
			g_props.emplace_back();
		}
		auto& p = g_props[id];
		p = Prop{};
		p.used = true;
		p.model = a_model;
		p.node = node;
		node->local = RE::NiTransform();
		g_root->AttachChild(node.get(), true);
		return id;
	}

	bool Valid(Id a_id) { return a_id >= 0 && a_id < static_cast<int>(g_props.size()) && g_props[a_id].used; }

	void Remove(Id a_id)
	{
		if (!Valid(a_id)) {
			return;
		}
		auto& p = g_props[a_id];
		if (p.node && g_root) {
			g_root->DetachChild2(p.node.get());
		}
		p = Prop{};
	}

	void RemoveAll()
	{
		for (int i = 0; i < static_cast<int>(g_props.size()); ++i) {
			Remove(i);
		}
		g_props.clear();
		Render3D::ForgetMeasures();
	}

	void Place(Id a_id, const hc::Vec3& a_pos, const hc::Quat& a_rot, float a_scale)
	{
		if (!Valid(a_id)) {
			return;
		}
		auto& p = g_props[a_id];
		p.node->local.translate = { static_cast<float>(a_pos.x), static_cast<float>(a_pos.y), static_cast<float>(a_pos.z) };
		p.node->local.rotate = FromQuat(a_rot);
		p.node->local.scale = a_scale;
		p.placed = true;
	}

	::ID3D11ShaderResourceView* FoodTexture(Id a_id)
	{
		if (!Valid(a_id) || !g_props[a_id].node) {
			return nullptr;
		}
		auto&                       p = g_props[a_id];
		::ID3D11ShaderResourceView* found = nullptr;
		int                         shapeIndex = -1;
		RE::BSVisit::TraverseScenegraphGeometries(p.node.get(), [&](RE::BSGeometry* a_geo) {
			++shapeIndex;
			if (!a_geo || found) {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			std::string n = a_geo->name.c_str() ? a_geo->name.c_str() : "";
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (n.find("bowl") != std::string::npos || n.find("plate") != std::string::npos || n.find("spoon") != std::string::npos) {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			::ID3D11ShaderResourceView* d = nullptr;
			for (const auto& sw : p.swaps) {
				const bool named = !sw.shape.empty() && _stricmp(sw.shape.c_str(), a_geo->name.c_str()) == 0;
				if (named || (sw.shape.empty() && sw.index == shapeIndex)) {
					d = SrvOfPath(sw.diffuse.c_str());
				}
			}
			if (!d) {
				auto* prop = a_geo->GetGeometryRuntimeData().shaderProperty.get();
				auto* lit = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
				auto* mat = lit ? static_cast<RE::BSLightingShaderMaterialBase*>(lit->material) : nullptr;
				if (mat) {
					d = SrvOf(mat->diffuseTexture.get());
					if (!d && mat->textureSet) {
						d = SrvOfPath(mat->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
					}
				}
			}
			found = d;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return found;
	}

	void HideShapes(Id a_id, std::vector<std::string> a_words)
	{
		if (Valid(a_id)) {
			g_props[a_id].hideShapes = std::move(a_words);
			g_props[a_id].measured = false;
			g_props[a_id].asked = false;
		}
	}

	void SwapTextures(Id a_id, const std::string& a_shape, int a_index, const std::string& a_diffuse, const std::string& a_normal)
	{
		if (Valid(a_id)) {
			auto full = [](const std::string& a_p) {
				if (a_p.empty()) {
					return a_p;
				}
				std::string l = a_p;
				std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return l.rfind("textures\\", 0) == 0 || l.rfind("textures/", 0) == 0 ? a_p : "textures\\" + a_p;
			};
			g_props[a_id].swaps.push_back({ a_shape, full(a_diffuse), full(a_normal), a_index });
		}
	}

	void Stretch(Id a_id, float a_x, float a_y, float a_z)
	{
		if (Valid(a_id)) {
			auto& p = g_props[a_id];
			p.scale3[0] = a_x;
			p.scale3[1] = a_y;
			p.scale3[2] = a_z;
		}
	}

	void StretchWorld(Id a_id, float a_x, float a_y, float a_z)
	{
		if (Valid(a_id)) {
			auto& p = g_props[a_id];
			p.world3[0] = a_x;
			p.world3[1] = a_y;
			p.world3[2] = a_z;
		}
	}

	void Hide(Id a_id, bool a_hidden)
	{
		if (Valid(a_id)) {
			g_props[a_id].hidden = a_hidden;
		}
	}

	Look& LookOf(Id a_id)
	{
		static Look s_none;
		return Valid(a_id) ? g_props[a_id].look : (s_none = Look{});
	}

	bool FlatBounds(Id a_id, hc::Vec3& a_min, hc::Vec3& a_max)
	{
		if (!Valid(a_id)) {
			return false;
		}
		auto& p = g_props[a_id];
		TryBounds(p);
		if (!p.measured) {
			return false;
		}
		a_min = p.fmin;
		a_max = p.fmax;
		return true;
	}

	bool Bounds(Id a_id, hc::Vec3& a_min, hc::Vec3& a_max)
	{
		if (!Valid(a_id)) {
			return false;
		}
		auto& p = g_props[a_id];
		TryBounds(p);
		if (!p.measured) {
			return false;
		}
		a_min = p.bmin;
		a_max = p.bmax;
		return true;
	}

	ID3D11ShaderResourceView* Skin(Id a_id) { return Valid(a_id) ? g_props[a_id].skin : nullptr; }
	ID3D11ShaderResourceView* Texture(const char* a_path) { return SrvOfPath(a_path); }

	void SetCamera(const hc::Vec3& a_target, float a_dist, bool a_snap)
	{
		g_camWant = a_target;
		g_camDistWant = a_dist;
		if (a_snap) {
			g_camAt = a_target;
			g_camDist = a_dist;
		}
	}

	void SetHaze(const hc::Vec3& a_pos, float a_amount)
	{
		g_hazePos = a_pos;
		g_haze = a_amount;
	}

	void SetFire(const hc::Vec3& a_pos, float a_heat)
	{
		g_firePos = a_pos;
		g_fireHeat = a_heat;
	}

	void SetSprites(std::vector<Render3D::Sprite>&& a_sprites) { g_sprites = std::move(a_sprites); }

	bool Project(const hc::Vec3& a_p, float& a_sx, float& a_sy)
	{
		if (!g_placed) {
			return false;
		}
		const float x = static_cast<float>(a_p.x), y = static_cast<float>(a_p.y), z = static_cast<float>(a_p.z);
		const float cx = g_vp[0] * x + g_vp[1] * y + g_vp[2] * z + g_vp[3];
		const float cy = g_vp[4] * x + g_vp[5] * y + g_vp[6] * z + g_vp[7];
		const float cw = g_vp[12] * x + g_vp[13] * y + g_vp[14] * z + g_vp[15];
		if (cw <= 0.01f) {
			return false;
		}
		a_sx = (cx / cw * 0.5f + 0.5f) * g_screenW;
		a_sy = (0.5f - cy / cw * 0.5f) * g_screenH;
		return std::isfinite(a_sx) && std::isfinite(a_sy);
	}

	bool Unproject(float a_sx, float a_sy, double a_planeZ, double& a_x, double& a_y)
	{
		if (!g_placed) {
			return false;
		}
		// the ray from the eye through the pixel, onto the plane z = a_planeZ
		const float aspect = g_screenW / (std::max)(1.0f, g_screenH);
		const float nx = (a_sx / g_screenW * 2.0f - 1.0f) * g_th * aspect;
		const float ny = (1.0f - a_sy / g_screenH * 2.0f) * g_th;
		const double d[3] = { g_fwd[0] + g_right[0] * nx + g_up[0] * ny, g_fwd[1] + g_right[1] * nx + g_up[1] * ny, g_fwd[2] + g_right[2] * nx + g_up[2] * ny };
		if (std::abs(d[2]) < 1e-6) {
			return false;
		}
		const double t = (a_planeZ - g_eye[2]) / d[2];
		if (t <= 0.0) {
			return false;
		}
		a_x = g_eye[0] + d[0] * t;
		a_y = g_eye[1] + d[1] * t;
		return true;
	}

	float PixelsPerUnit(const hc::Vec3& a_at)
	{
		float ax = 0, ay = 0, bx = 0, by = 0;
		if (!Project(a_at, ax, ay) || !Project({ a_at.x + 1.0, a_at.y, a_at.z }, bx, by)) {
			return 10.0f;
		}
		return std::hypot(bx - ax, by - ay);
	}

	void Update(float a_dt)
	{
		if (!g_open || !g_root) {
			return;
		}
		Camera(a_dt);
		RE::NiUpdateData ud;
		g_root->Update(ud);

		Render3D::Frame f;
		f.show = true;
		const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_shownAt).count();
		f.blur = std::clamp(since / 0.4f, 0.0f, 1.0f);
		f.reveal = std::clamp((since - 0.1f) / 0.6f, 0.0f, 1.0f);
		std::memcpy(f.viewProj, g_vp, sizeof(g_vp));
		std::memcpy(f.camPos, g_eye, sizeof(g_eye));
		std::memcpy(f.camRight, g_right, sizeof(g_right));
		std::memcpy(f.camUp, g_up, sizeof(g_up));
		f.shadowCenter[0] = 0.0f;
		f.shadowCenter[1] = 0.0f;
		f.shadowHalf[0] = 44.0f;
		f.shadowHalf[1] = 30.0f;
		f.fire[0] = static_cast<float>(g_firePos.x);
		f.fire[1] = static_cast<float>(g_firePos.y);
		f.fire[2] = static_cast<float>(g_firePos.z);
		// the fire's glow: reaching further and brighter as it grows (onto the pot, the board, the dough)
		f.fire[3] = 40.0f + 40.0f * std::clamp(g_fireHeat, 0.0f, 1.3f);
		const float fh = std::clamp(g_fireHeat, 0.0f, 1.4f);
		f.fireColor[0] = 1.0f * fh * 2.2f;
		f.fireColor[1] = 0.44f * fh * 2.2f;
		f.fireColor[2] = 0.13f * fh * 2.2f;
		f.fireHeat = fh;
		f.haze[0] = static_cast<float>(g_hazePos.x);
		f.haze[1] = static_cast<float>(g_hazePos.y);
		f.haze[2] = static_cast<float>(g_hazePos.z);
		f.haze[3] = g_haze;
		f.sprites = g_sprites;
		for (auto& p : g_props) {
			if (!p.used || !p.placed || p.hidden || !p.node) {
				continue;
			}
			TryBounds(p);
			int shapeIndex = -1;
			RE::BSVisit::TraverseScenegraphGeometries(p.node.get(), [&](RE::BSGeometry* a_geo) {
				++shapeIndex;
				auto* tri = a_geo ? a_geo->AsTriShape() : nullptr;
				if (!tri || !Visible(a_geo, p.node.get()) || LeftOut(p, a_geo)) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				if (p.look.onlyFlat && p.measured && p.flatIndex >= 0 && shapeIndex != p.flatIndex) {
					return RE::BSVisit::BSVisitControl::kContinue;  // (its bowl)
				}
				if (p.look.hideFlat && p.measured && p.flatIndex >= 0 && shapeIndex == p.flatIndex) {
					return RE::BSVisit::BSVisitControl::kContinue;  // (the stew modelled in a game pot)
				}
				auto& gd = a_geo->GetGeometryRuntimeData();
				auto* rd = gd.rendererData;
				auto* prop = gd.shaderProperty.get();
				auto* lit = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
				auto* mat = lit ? static_cast<RE::BSLightingShaderMaterialBase*>(lit->material) : nullptr;
				Buffers bufs;
				(void)rd;
				if (!mat || !BuffersOf(a_geo, bufs)) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				Render3D::Item it;
				it.vb = bufs.vb;
				it.ib = bufs.ib;
				it.desc = bufs.desc;
				it.indexCount = bufs.indexCount;
				it.vertexCount = bufs.vertexCount;
				it.name = a_geo->name.c_str();
				// The geometry's place, with the prop's stretches: along its own axes (inside its rotation) and along the
				// kitchen's (outside it). One matrix, not a pure rotation: world = W R S (rel) * s + t.
				{
					float rel[9], relPos[3], relScale = 1.0f;
					Relative(p.node->world, a_geo->world, rel, relPos, relScale);
					const auto& pw = p.node->world;
					const float ps = pw.scale;
					float       m[9];
					for (int r = 0; r < 3; ++r) {
						for (int c = 0; c < 3; ++c) {
							float v = 0.0f;
							for (int k = 0; k < 3; ++k) {
								v += pw.rotate.entry[r][k] * p.scale3[k] * rel[k * 3 + c];
							}
							m[r * 3 + c] = p.world3[r] * v * relScale * ps;
						}
					}
					float t[3];
					for (int r = 0; r < 3; ++r) {
						float v = 0.0f;
						for (int k = 0; k < 3; ++k) {
							v += pw.rotate.entry[r][k] * p.scale3[k] * relPos[k];
						}
						t[r] = p.world3[r] * v * ps;
					}
					std::memcpy(it.rot, m, sizeof(m));
					it.pos[0] = pw.translate.x + t[0];
					it.pos[1] = pw.translate.y + t[1];
					it.pos[2] = pw.translate.z + t[2];
					it.scale = 1.0f;
				}
				auto* set = mat->textureSet.get();
				it.diffuse = SrvOf(mat->diffuseTexture.get());
				if (!it.diffuse && set) {
					it.diffuse = SrvOfPath(set->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
				}
				bool swapped = false;
				// the form's own textures for this shape (the game's stews share one mesh, told apart this way)
				for (const auto& sw : p.swaps) {
					const bool named = !sw.shape.empty() && _stricmp(sw.shape.c_str(), a_geo->name.c_str()) == 0;
					if (named || (sw.shape.empty() && sw.index == shapeIndex)) {
						if (auto* d = SrvOfPath(sw.diffuse.c_str())) {
							it.diffuse = d;
							swapped = true;
						}
						if (auto* n = SrvOfPath(sw.normal.c_str())) {
							it.normal = n;
						}
					}
				}
				if (!p.skin && it.diffuse) {
					p.skin = it.diffuse;
				}
				it.normal = SrvOf(mat->normalTexture.get());
				if (!it.normal && set) {
					it.normal = SrvOfPath(set->GetTexturePath(RE::BSTextureSet::Texture::kNormal));
				}
				if (mat->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap) {
					auto* env = static_cast<RE::BSLightingShaderMaterialEnvmap*>(mat);
					it.env = SrvOf(env->envTexture.get());
					it.envMask = SrvOf(env->envMaskTexture.get());
					it.envScale = env->envMapScale;
				}
				const bool specular = lit->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSpecular);
				it.specColor[0] = mat->specularColor.red;
				it.specColor[1] = mat->specularColor.green;
				it.specColor[2] = mat->specularColor.blue;
				it.specScale = specular ? mat->specularColorScale : 0.0f;
				it.gloss = mat->specularPower;
				it.alpha = mat->materialAlpha;
				if (auto* ap = gd.alphaProperty.get()) {
					it.blend = ap->GetAlphaBlending();
					it.test = ap->GetAlphaTesting();
					it.alphaRef = ap->alphaThreshold / 255.0f;
				}
				if (swapped) {
					// a swapped texture's alpha needn't be what the shape's own material cuts by (the game's cabbage
					// wears one and vanished): no cut-out on it (after the shape's own alpha settings, or they win)
					it.test = false;
					it.blend = false;
				}
				const auto& lk = p.look;
				if (lk.skin) {
					it.diffuse = lk.skin;
					it.normal = nullptr;
				}
				std::memcpy(it.tint, lk.tint, sizeof(it.tint));
				std::memcpy(it.lift, lk.lift, sizeof(it.lift));
				std::memcpy(it.emissive, lk.emissive, sizeof(it.emissive));
				std::memcpy(it.clips, lk.clips, sizeof(it.clips));
				std::memcpy(it.flesh, lk.flesh, sizeof(it.flesh));
				std::memcpy(it.shape, lk.shape, sizeof(it.shape));
				std::memcpy(it.uv, lk.uv, sizeof(it.uv));
				it.cookMode = lk.cookMode;
				std::memcpy(it.cook, lk.cook, sizeof(it.cook));
				std::memcpy(it.cookAxis, lk.cookAxis, sizeof(it.cookAxis));
				std::memcpy(it.cookRef, lk.cookRef, sizeof(it.cookRef));
				it.shadow = lk.shadow;
				it.fleshTex = lk.fleshTex;
				it.sear = lk.sear;
				it.unshadowed = lk.unshadowed;
				std::memcpy(it.hollow, lk.hollow, sizeof(it.hollow));
				it.hollowDepth = lk.hollowDepth;
				std::memcpy(it.fx, lk.fx, sizeof(it.fx));
				std::memcpy(it.spot, lk.spot, sizeof(it.spot));
				std::memcpy(it.fx2, lk.fx2, sizeof(it.fx2));
				if (lk.alpha < 0.999f) {
					it.alpha = lk.alpha;
					it.blend = true;
					it.fade = true;
					it.test = false;
					it.shadow = false;
				}
				f.items.push_back(std::move(it));
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		Render3D::Submit(std::move(f));
	}
}
