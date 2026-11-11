#version 330 core

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform sampler2D texture0;
uniform vec4      colDiffuse;

// Fog uniforms — встановлюються з C++ через SetShaderValue
uniform float fogNear;   // початок туману (метри)
uniform float fogFar;    // кінець туману (метри)
uniform vec3  fogColor;  // колір туману (відповідає біому)
uniform vec3  cameraPos; // позиція камери

out vec4 finalColor;

void main()
{
    vec4 texColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;

    // Відстань від камери до фрагмента
    float dist = length(fragPosition - cameraPos);

    // Лінійний туман: 0.0 = немає туману, 1.0 = повний туман
    float fogFactor = clamp((dist - fogNear) / (fogFar - fogNear), 0.0, 1.0);

    // Змішуємо колір об'єкта з кольором туману
    finalColor = mix(texColor, vec4(fogColor, texColor.a), fogFactor);
}
