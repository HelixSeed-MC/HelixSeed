package GPULootSeedFinder.ui;

import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.time.Duration;

public class TextureDownloader {
    private final HttpClient client = HttpClient.newBuilder()
        .followRedirects(HttpClient.Redirect.NORMAL)
        .connectTimeout(Duration.ofSeconds(10))
        .build();
    private final Path textureDir;

    public TextureDownloader() {
        this.textureDir = Path.of(System.getProperty("user.home"), ".gpulootseedfinder", "textures");
    }

    public Path getTexturePath(StructureDefinition def) {
        return textureDir.resolve(def.id + ".png");
    }

    public Path ensureTextureDownloaded(StructureDefinition def) throws IOException, InterruptedException {
        Files.createDirectories(textureDir);
        Path target = getTexturePath(def);
        if (Files.exists(target) && Files.size(target) > 0) {
            return target;
        }
        HttpRequest request = HttpRequest.newBuilder()
            .GET()
            .timeout(Duration.ofSeconds(20))
            .uri(URI.create(def.textureUrl))
            .build();
        HttpResponse<InputStream> response = client.send(request, HttpResponse.BodyHandlers.ofInputStream());
        if (response.statusCode() < 200 || response.statusCode() >= 300) {
            throw new IOException("Texture download failed for " + def.id + " (HTTP " + response.statusCode() + ")");
        }
        try (InputStream in = response.body()) {
            Files.copy(in, target, StandardCopyOption.REPLACE_EXISTING);
        }
        return target;
    }
}

