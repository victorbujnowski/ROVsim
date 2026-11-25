using UnrealBuildTool;

// Módulo só de EDITOR: ferramentas que criam assets por código (não vai para o jogo empacotado)
public class ROVsimTools : ModuleRules
{
	public ROVsimTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// Niagara (o asset), NiagaraEditor (criar sistema e adicionar emissor), UnrealEd (commandlet), AssetRegistry
		PrivateDependencyModuleNames.AddRange(new string[] { "Niagara", "NiagaraEditor", "UnrealEd", "AssetRegistry" });
	}
}
