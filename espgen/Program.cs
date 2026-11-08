// MiniSkirt.esp を作る（MCM Helper 用のクエスト 1 つだけ）
// 使い方: dotnet run -- <出力フォルダ>          … <出力フォルダ>/MiniSkirt.esp と SEQ/MiniSkirt.seq を書く
//         dotnet run -- --dump <esp のパス>      … ほかの ESP のクエストの中身を表示する（作り方の参考）
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Skyrim;

const SkyrimRelease Rel = SkyrimRelease.SkyrimVR;

if (args.Length > 1 && args[0] == "--dump")
{
	using var other = SkyrimMod.CreateFromBinaryOverlay(args[1], Rel);
	Console.WriteLine($"header flags={other.ModHeader.Flags} version={other.ModHeader.Stats.Version}");
	foreach (var q in other.Quests)
	{
		Console.WriteLine($"QUST {q.FormKey} {q.EditorID} flags={q.Flags} priority={q.Priority} type={q.Type}");
		var vm = q.VirtualMachineAdapter;
		if (vm != null)
		{
			foreach (var s in vm.Scripts)
			{
				Console.WriteLine($"  script {s.Name} flags={s.Flags}");
				foreach (var p in s.Properties) Console.WriteLine($"    prop {p.Name} {p.GetType().Name} {(p as IScriptStringPropertyGetter)?.Data}");
			}
			foreach (var a in vm.Aliases)
			{
				Console.WriteLine($"  alias-scripts alias={a.Property.Alias} obj={a.Property.Object.FormKey}");
				foreach (var s in a.Scripts) Console.WriteLine($"    script {s.Name}");
			}
		}
		foreach (var a in q.Aliases)
		{
			Console.WriteLine($"  alias {a.ID} {a.Name} type={a.Type} flags={a.Flags} forced={a.ForcedReference.FormKeyNullable}");
		}
	}
	return;
}

if (args.Length > 1 && args[0] == "--armo")
{
	// ほかの ESP の防具と ArmorAddon の中身（下着の作りを調べる用）
	using var other = SkyrimMod.CreateFromBinaryOverlay(args[1], Rel);
	foreach (var a in other.Armors)
	{
		var kws = a.Keywords == null ? "" : string.Join(",", a.Keywords.Select(k => k.FormKey.ToString()));
		Console.WriteLine($"ARMO {a.FormKey} {a.EditorID} name={a.Name?.String} slots={a.BodyTemplate?.FirstPersonFlags} type={a.BodyTemplate?.ArmorType} kw=[{kws}] arma=[{string.Join(",", a.Armature.Select(x => x.FormKey.ToString()))}]");
	}
	foreach (var a in other.ArmorAddons)
	{
		Console.WriteLine($"ARMA {a.FormKey} {a.EditorID} slots={a.BodyTemplate?.FirstPersonFlags} female={a.WorldModel?.Female?.File} race={a.Race.FormKey}");
	}
	foreach (var k in other.Keywords) Console.WriteLine($"KYWD {k.FormKey} {k.EditorID}");
	return;
}

var outDir = args.Length > 0 ? args[0] : ".";
Directory.CreateDirectory(outDir);
var modKey = ModKey.FromNameAndExtension("MiniSkirt.esp");
var skyrim = ModKey.FromNameAndExtension("Skyrim.esm");
var mod = new SkyrimMod(modKey, Rel);
mod.ModHeader.Stats.Version = 1.7f;  // VR は 1.70 形式
mod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;  // ESL フラグ（VR では ESL 対応 MOD があるときだけ効く。無ければ普通の ESP として読まれる）

// MCM Helper の設定画面のクエスト。スクリプトは MCM Helper の MCM_ConfigBase をそのまま付ける（自前のスクリプトは要らない。
// 値は MCM Helper が Data\MCM\Settings\MiniSkirt.ini に書き、DLL がメニューを閉じたときに読み直す）
var quest = new Quest(new FormKey(modKey, 0x800), Rel) { EditorID = "MiniSkirt_MCMQuest" };
quest.Name = "MiniSkirt";
quest.Flags = Quest.Flag.StartGameEnabled;
quest.Priority = 50;
quest.Aliases.Add(new QuestAlias
{
	ID = 0,
	Name = "PlayerAlias",
	Type = QuestAlias.TypeEnum.Reference,
	ForcedReference = new FormLinkNullable<IPlacedGetter>(new FormKey(skyrim, 0x000014)),  // PlayerRef
});
quest.NextAliasID = 1;
quest.VirtualMachineAdapter = new QuestAdapter
{
	Scripts =
	{
		new ScriptEntry
		{
			Name = "MCM_ConfigBase",
			Properties = { new ScriptStringProperty { Name = "ModName", Data = "MiniSkirt" } },
		},
	},
	Aliases =
	{
		new QuestFragmentAlias
		{
			Property = new ScriptObjectProperty { Object = quest.ToLink<ISkyrimMajorRecordGetter>(), Alias = 0 },
			Scripts = { new ScriptEntry { Name = "SKI_PlayerLoadGameAlias" } },
		},
	},
};
mod.Quests.Add(quest);
mod.ModHeader.Stats.NextFormID = 0x801;

var espPath = Path.Combine(outDir, "MiniSkirt.esp");
mod.WriteToBinary(espPath, new BinaryWriteParameters
{
	MastersListOrdering = new MastersListOrderingByLoadOrder(new[] { skyrim, modKey }),
});

// SEQ: ゲーム開始時に有効なクエストの FormID（ファイルの中の FormID。上位 8 ビットはマスターの数 = 1）
var seqDir = Path.Combine(outDir, "SEQ");
Directory.CreateDirectory(seqDir);
using (var w = new BinaryWriter(File.Create(Path.Combine(seqDir, "MiniSkirt.seq"))))
{
	w.Write(0x01000000u | quest.FormKey.ID);
}
Console.WriteLine($"書き出し: {espPath}");
