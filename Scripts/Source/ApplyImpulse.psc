Scriptname ApplyImpulse

; Aplica impulso customizado em um ator.
; x/y usam a direcao local do ator, z usa o eixo vertical do mundo.
Function ApplyCustomVelocityImpulse(Actor akActor, Float x, Float y, Float z, Float time, Bool inflictDamage = True) Global Native

; Aplica impulso customizado com controle de momentum por canal.
; allowMomentumHorizontal controla soma/substituicao de x/y.
; allowMomentumVertical controla soma/substituicao de z.
Function ApplyCustomVelocityImpulseMomentum(Actor akActor, Float x, Float y, Float z, Float time, Bool inflictDamage = True, Bool allowMomentumHorizontal = True, Bool allowMomentumVertical = True) Global Native

; Aplica rotacao yaw customizada em graus.
Function ApplyCustomRotation(Actor akActor, Float yawDegrees, Float time) Global Native

; Retorna se o ator esta temporariamente protegido contra dano de colisao/queda gerado pelo impulso.
Bool Function IsCollisionDamageSuppressed(Actor akActor) Global Native
