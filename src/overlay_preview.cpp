#include "overlay_preview.h"

// Material swap, as RaceMenu (and other plugins that change a shape's textures at runtime) do it: copy the shape's
// lighting material, give the copy the new texture set and values, hand it to the shader property (which makes its own
// copy), set the geometry up again, and free ours.

namespace slavetats_ng
{
	namespace overlay_preview
	{
		namespace
		{
			// What a shape looked like before the first preview touched it
			struct Saved
			{
				RE::NiPointer<RE::BSGeometry> geometry;  // kept alive: a 3D reload may drop it meanwhile
				RE::NiPointer<RE::BSTextureSet> textureSet;
				float                           alpha = 1.0f;
				float                           specularPower = 0.0f;
				float                           specularScale = 0.0f;
				bool                            hasTint = false;
				RE::NiColor                     tint;
				bool                            hasEmissive = false;
				RE::NiColor                     emissive;
				float                           emissiveMult = 1.0f;
			};
			std::vector<Saved> SavedShapes;  // game thread

			RE::BSLightingShaderProperty* LightingShader(RE::BSGeometry* a_geometry)
			{
				auto& effect = a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect];
				return effect ? netimmerse_cast<RE::BSLightingShaderProperty*>(effect.get()) : nullptr;
			}

			RE::NiColor ToColor(int a_rgb)
			{
				RE::NiColor c;
				c.red = ((a_rgb >> 16) & 0xFF) / 255.0f;
				c.green = ((a_rgb >> 8) & 0xFF) / 255.0f;
				c.blue = (a_rgb & 0xFF) / 255.0f;
				return c;
			}

			bool IsTint(RE::BSShaderMaterial* a_material)
			{
				auto feature = a_material->GetFeature();
				return feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint || feature == RE::BSShaderMaterial::Feature::kHairTint;
			}

			// The tint color lives at the same place in both tint materials
			RE::NiColor& TintOf(RE::BSShaderMaterial* a_material)
			{
				return static_cast<RE::BSLightingShaderMaterialFacegenTint*>(a_material)->tintColor;
			}

			// The new look of one shape: a texture set (null = keep the current one) and the material values
			struct Look
			{
				RE::NiPointer<RE::BSTextureSet> textureSet;
				float                           alpha = 1.0f;
				float                           specularPower = 0.0f;
				float                           specularScale = 0.0f;
				bool                            setTint = false;
				RE::NiColor                     tint;
				bool                            setEmissive = false;
				RE::NiColor                     emissive;
				float                           emissiveMult = 1.0f;
			};

			void Apply(RE::BSGeometry* a_geometry, const Look& a_look)
			{
				auto shader = LightingShader(a_geometry);
				auto base = shader ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
				if (!base)
					return;
				auto copy = static_cast<RE::BSLightingShaderMaterialBase*>(base->Create());
				if (!copy)
					return;
				copy->CopyMembers(base);
				if (a_look.textureSet) {
					copy->ClearTextures();
					copy->OnLoadTextureSet(0, a_look.textureSet.get());
				}
				copy->materialAlpha = a_look.alpha;
				copy->specularPower = a_look.specularPower;
				copy->specularColorScale = a_look.specularScale;
				if (a_look.setTint && IsTint(copy))
					TintOf(copy) = a_look.tint;
				shader->SetMaterial(copy, true);  // the property keeps its own copy
				shader->SetupGeometry(a_geometry);
				shader->FinishSetupGeometry(a_geometry);
				copy->~BSLightingShaderMaterialBase();
				RE::free(copy);

				if (a_look.setEmissive && shader->emissiveColor) {
					*shader->emissiveColor = a_look.emissive;
					shader->emissiveMult = a_look.emissiveMult;
				}
			}

			void Remember(RE::BSGeometry* a_geometry)
			{
				for (auto& saved : SavedShapes) {
					if (saved.geometry.get() == a_geometry)
						return;  // the first look is the one to go back to
				}
				auto shader = LightingShader(a_geometry);
				auto base = shader ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
				if (!base)
					return;
				Saved saved;
				saved.geometry.reset(a_geometry);
				saved.textureSet = base->textureSet;
				saved.alpha = base->materialAlpha;
				saved.specularPower = base->specularPower;
				saved.specularScale = base->specularColorScale;
				if (IsTint(base)) {
					saved.hasTint = true;
					saved.tint = TintOf(base);
				}
				if (shader->emissiveColor) {
					saved.hasEmissive = true;
					saved.emissive = *shader->emissiveColor;
					saved.emissiveMult = shader->emissiveMult;
				}
				SavedShapes.push_back(std::move(saved));
			}

			// The shapes an overlay name stands for: the geometry itself, or the ones under a node of that name
			void Collect(RE::NiAVObject* a_object, std::vector<RE::BSGeometry*>& a_out)
			{
				if (!a_object)
					return;
				if (auto geometry = a_object->AsGeometry()) {
					a_out.push_back(geometry);
					return;
				}
				if (auto node = a_object->AsNode()) {
					for (auto& child : node->GetChildren())
						Collect(child.get(), a_out);
				}
			}

			RE::NiPointer<RE::BSTextureSet> TextureSetWith(RE::BSGeometry* a_geometry, const std::string& a_texture)
			{
				auto shader = LightingShader(a_geometry);
				auto base = shader ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
				auto set = RE::BSShaderTextureSet::Create();
				if (!set)
					return {};
				using Texture = RE::BSTextureSet::Texture;
				if (base && base->textureSet) {
					for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Texture::kTotal); i++) {
						auto t = static_cast<Texture>(i);
						if (auto path = base->textureSet->GetTexturePath(t))
							set->SetTexturePath(t, path);
					}
				}
				// Shader texture paths start at Data: "textures\..."
				std::string path = a_texture;
				if (_strnicmp(path.c_str(), "textures\\", 9) != 0 && _strnicmp(path.c_str(), "textures/", 9) != 0)
					path = "textures\\" + path;
				set->SetTexturePath(Texture::kDiffuse, path.c_str());
				return RE::NiPointer<RE::BSTextureSet>(set);
			}
		}

		bool Show(RE::Actor* a_actor, const char* a_area, int a_slot, const std::string& a_texture, int a_color, int a_glow, bool a_gloss)
		{
			auto root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root)
				return false;
			RE::BSFixedString name = fmt::format("{} [Ovl{}]", a_area, a_slot);
			std::vector<RE::BSGeometry*> shapes;
			Collect(root->GetObjectByName(name), shapes);
			if (shapes.empty())
				return false;

			for (auto geometry : shapes) {
				Remember(geometry);
				Look look;
				if (a_texture.empty()) {
					look.alpha = 0.0f;  // an empty slot: RaceMenu's blank texture, invisible
				} else {
					look.textureSet = TextureSetWith(geometry, a_texture);
					look.alpha = 1.0f;
					look.specularPower = a_gloss ? 5.0f : 0.0f;  // what SlaveTats uses for its boolean gloss
					look.specularScale = a_gloss ? 5.0f : 0.0f;
					look.setTint = true;
					look.tint = ToColor(a_color);
					look.setEmissive = true;
					look.emissive = ToColor(a_glow);
					look.emissiveMult = 1.0f;
				}
				Apply(geometry, look);
			}
			return true;
		}

		bool RestoreAll()
		{
			bool any = !SavedShapes.empty();
			for (auto& saved : SavedShapes) {
				Look look;
				look.textureSet = saved.textureSet;
				look.alpha = saved.alpha;
				look.specularPower = saved.specularPower;
				look.specularScale = saved.specularScale;
				look.setTint = saved.hasTint;
				look.tint = saved.tint;
				look.setEmissive = saved.hasEmissive;
				look.emissive = saved.emissive;
				look.emissiveMult = saved.emissiveMult;
				Apply(saved.geometry.get(), look);
			}
			SavedShapes.clear();
			return any;
		}

		void Forget()
		{
			SavedShapes.clear();
		}

		void LogShapes(RE::Actor* a_actor, const char* a_area, int a_count)
		{
			auto root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				logger::info("Overlays: {} has no 3D", a_area);
				return;
			}
			for (int slot = 0; slot < a_count; slot++) {
				RE::BSFixedString name = fmt::format("{} [Ovl{}]", a_area, slot);
				std::vector<RE::BSGeometry*> shapes;
				Collect(root->GetObjectByName(name), shapes);
				if (shapes.empty()) {
					logger::info("Overlays: {} [Ovl{}]: no shape", a_area, slot);
					continue;
				}
				for (auto geometry : shapes) {
					auto shader = LightingShader(geometry);
					auto base = shader ? static_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
					const char* diffuse = base && base->textureSet ? base->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
					bool held = false;
					for (auto& saved : SavedShapes)
						held |= saved.geometry.get() == geometry;
					logger::info("Overlays: {} [Ovl{}]: alpha {:.2f}, shader alpha {:.2f}, culled {}, diffuse texture loaded {}, preview holds it {}, '{}'",
						a_area, slot, base ? base->materialAlpha : -1.0f, shader ? shader->alpha : -1.0f, geometry->GetAppCulled(),
						base && base->diffuseTexture ? "yes" : "no", held, diffuse ? diffuse : "(none)");
				}
			}
		}
	}
}
