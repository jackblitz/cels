# CELS Motion & Temporal Transition Specification

> **CELS Motion & Transition Specification**  
> Mathematical models, ordinary differential equations (ODEs), velocity transfer, and convergence invariants for declarative motion systems.

---

## 1. Motion Architecture & Separation of Concerns

Interactive declarative systems require smooth temporal convergence between discrete, authoritative application state and continuous, ephemeral presentation state:
- Health bars interpolating toward updated hit point values.
- UI elements sliding into position upon menu selection.
- Camera targets tracking dynamic player movement with damping and elastic overshoot.

### 1.1 Two-Tier Architecture

CELS strictly decouples motion into two distinct architectural layers:

```
┌────────────────────────────────────────────────────────────────────────┐
│                      Domain Motion Layer (celm_)                       │
│  - Damped Harmonic Springs (Stiffness, Damping, Mass)                  │
│  - Exact Analytical ODE Solvers (Underdamped, Critically Damped)       │
│  - Cubic Bézier Curve Solvers (Newton-Raphson root finding)            │
│  - Frame-Rate Independent Exponential Smoothing (celm_damp)            │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Built on top of
┌───────────────────────────────────▼────────────────────────────────────┐
│                    Core Engine Primitive (cels_)                       │
│  - cel_transition(targetValue, durationMs, easeFn)                     │
│  - Slot-Memory Temporal State (CelsTransitionState)                    │
│  - Automatic Session Invalidation & Recomposition Scheduling           │
│  - Pure C99, Zero Heap Allocations, 32-Byte Cache Alignment            │
└────────────────────────────────────────────────────────────────────────┘
```

1. **Core Transition Layer (`include/cels/runtime/transition.h`)**:
   Provides the fundamental temporal convergence primitive: [`cel_transition()`](file:///D:/cels-workspace/library/cels/include/cels/transition.h). Operates on monotonic clock elapsed time, evaluates standard easing curves, handles in-flight retargeting, and automatically flags the session for recomposition while in motion.
2. **Domain Motion Layer (`celm_`)**:
   Higher-level companion library implementing physics-based models (springs, decay, bezier solvers) on top of CELS slot memory and temporal hooks.

---

## 2. Standard Easing Curves Specification

Easing functions map a normalized time parameter $t \in [0.0, 1.0]$ to an eased progress value $f(t) \in \mathbb{R}$:

$$\text{CelsEasingFn}: [0.0, 1.0] \rightarrow \mathbb{R}$$

Standard boundary conditions require $f(0) = 0.0$ and $f(1) = 1.0$ (with elastic curves permitted to exceed $[0, 1]$ during oscillation).

### 2.1 Easing Function Formulations

| Curve Identifier | Function Formulation $f(t)$ for $t \in [0, 1]$ | Initial Velocity $\dot{f}(0)$ | Final Velocity $\dot{f}(1)$ |
| :--- | :--- | :--- | :--- |
| **`CEL_EASE_LINEAR`** | $f(t) = t$ | $1$ | $1$ |
| **`CEL_EASE_IN_QUAD`** | $f(t) = t^2$ | $0$ | $2$ |
| **`CEL_EASE_OUT_QUAD`** | $f(t) = 1 - (1 - t)^2 = t(2 - t)$ | $2$ | $0$ |
| **`CEL_EASE_IN_OUT_QUAD`** | $f(t) = \begin{cases} 2t^2 & t < 0.5 \\ 1 - \frac{(-2t + 2)^2}{2} & t \ge 0.5 \end{cases}$ | $0$ | $0$ |
| **`CEL_EASE_IN_CUBIC`** | $f(t) = t^3$ | $0$ | $3$ |
| **`CEL_EASE_OUT_CUBIC`** | $f(t) = 1 - (1 - t)^3$ | $3$ | $0$ |
| **`CEL_EASE_IN_OUT_CUBIC`**| $f(t) = \begin{cases} 4t^3 & t < 0.5 \\ 1 - \frac{(-2t + 2)^3}{2} & t \ge 0.5 \end{cases}$ | $0$ | $0$ |
| **`CEL_EASE_IN_SINE`** | $f(t) = 1 - \cos\left(\frac{t \pi}{2}\right)$ | $0$ | $\frac{\pi}{2} \approx 1.571$ |
| **`CEL_EASE_OUT_SINE`** | $f(t) = \sin\left(\frac{t \pi}{2}\right)$ | $\frac{\pi}{2} \approx 1.571$ | $0$ |
| **`CEL_EASE_IN_OUT_SINE`** | $f(t) = -\frac{1}{2} \left(\cos(\pi t) - 1\right)$ | $0$ | $0$ |
| **`CEL_EASE_OUT_BACK`** | $f(t) = 1 + c_1(t - 1)^3 + c_3(t - 1)^2$<br>where $c_1 = 1.70158$, $c_3 = c_1 + 1 = 2.70158$ | $c_3 - c_1 = 1$ | $0$ |
| **`CEL_EASE_OUT_BOUNCE`** | Piecewise parabolic decay (detailed below) | $0$ | $0$ |

#### Out Bounce Formulation

The bounce curve models decaying parabolic gravity bounces:

$$f(t) = \begin{cases}
n_1 t^2 & t < \frac{1}{d_1} \\
n_1 (t - \frac{1.5}{d_1})^2 + 0.75 & t < \frac{2}{d_1} \\
n_1 (t - \frac{2.25}{d_1})^2 + 0.9375 & t < \frac{2.625}{d_1} \\
n_1 (t - \frac{2.625}{d_1})^2 + 0.984375 & \text{otherwise}
\end{cases}$$

where constants are $n_1 = 7.5625$ and $d_1 = 2.75$.

---

## 3. Temporal Value Transitions (`cel_transition`)

### 3.1 State Representation

Each `cel_transition` invocation retains persistent state in the session's slot memory:

```c
typedef struct CelsTransitionState {
    uint64_t     startTimeMs;    /**< Timestamp when transition was initiated (8 B) */
    CelsEasingFn easing;         /**< Active easing curve callback (8 B) */
    float        current;        /**< Current interpolated value (4 B) */
    float        startVal;       /**< Value when transition started (4 B) */
    float        targetVal;      /**< Destination target value (4 B) */
    uint32_t     durationMs;     /**< Transition duration in milliseconds (4 B) */
    bool         isInitialized;  /**< True once initial value has been established (1 B) */
    bool         isSettled;      /**< True when current == targetVal and idle (1 B) */
} CelsTransitionState;
```

### 3.2 Interpolation Step

Given current monotonic time $t_{\text{now}}$:

1. **Normalized Time Progress**:
   $$\tau = \text{clamp}\left(\frac{t_{\text{now}} - t_{\text{start}}}{\Delta t}, 0.0, 1.0\right)$$
2. **Current Interpolated Value**:
   $$x(t_{\text{now}}) = x_{\text{start}} + (x_{\text{target}} - x_{\text{start}}) \cdot f(\tau)$$

### 3.3 In-Flight Retargeting & Continuity

When the target value changes mid-flight ($x_{\text{target, new}} \neq x_{\text{target}}$):
1. The transition evaluates its current position:
   $$x_{\text{current}} = x(t_{\text{now}})$$
2. The current position becomes the new starting baseline:
   $$x_{\text{start}} \leftarrow x_{\text{current}}, \quad t_{\text{start}} \leftarrow t_{\text{now}}$$
3. The new target is stored:
   $$x_{\text{target}} \leftarrow x_{\text{target, new}}$$

This guarantees **$C^0$ positional continuity**: the displayed position never exhibits visual pops or teleportation across rapid target updates.

---

## 4. Spring ODE (Damped Harmonic Oscillator)

A physical spring system does not operate on a fixed time duration $\Delta t$; its convergence time and trajectory emerge naturally from second-order physical parameters:
- **Mass** $m > 0$ (inertial resistance, default $m = 1.0$)
- **Stiffness** $k > 0$ (restoring force / tension)
- **Damping** $c \ge 0$ (viscous resistance / friction)

### 4.1 Governing Differential Equation

$$m \frac{d^2 x}{dt^2} + c \frac{dx}{dt} + k (x - x_{\text{target}}) = 0$$

Let displacement from equilibrium be $y(t) = x(t) - x_{\text{target}}$. Substituting into the homogeneous ODE:

$$\ddot{y} + \frac{c}{m} \dot{y} + \frac{k}{m} y = 0$$

Define:
- **Undamped Angular Frequency**: $\omega_0 = \sqrt{\frac{k}{m}}$
- **Damping Ratio**: $\zeta = \frac{c}{2\sqrt{km}}$

The characteristic equation is:

$$s^2 + 2\zeta \omega_0 s + \omega_0^2 = 0$$

with roots:

$$s_{1, 2} = -\zeta \omega_0 \pm \omega_0 \sqrt{\zeta^2 - 1}$$

### 4.2 Analytical Solutions by Damping Regime

To avoid numerical instability, drift, and frame-rate dependence associated with explicit Euler integration, CELS evaluates the exact closed-form analytical solutions:

```
               Damping Ratio (zeta)
  0 ────────────────── 1.0 ──────────────────> +inf
   [  Underdamped   ]   [ Critically ] [  Overdamped   ]
   (Oscillates with)    (Damped: Min ) (Sluggish decay )
   (  bounce decay )    (time, no pop) (without bounce )
```

#### Regime 1: Underdamped ($\zeta < 1.0$)

The roots are complex conjugates: $s_{1, 2} = -\zeta \omega_0 \pm i \omega_d$, where:

$$\omega_d = \omega_0 \sqrt{1 - \zeta^2} \quad \text{(damped natural frequency)}$$

The closed-form displacement and velocity solutions are:

$$\begin{aligned}
x(t) &= x_{\text{target}} + e^{-\zeta \omega_0 t} \left( c_1 \cos(\omega_d t) + c_2 \sin(\omega_d t) \right) \\
v(t) &= \dot{x}(t) = e^{-\zeta \omega_0 t} \left( (c_2 \omega_d - c_1 \zeta \omega_0) \cos(\omega_d t) - (c_1 \omega_d + c_2 \zeta \omega_0) \sin(\omega_d t) \right)
\end{aligned}$$

Given initial conditions at $t = 0$: displacement $y_0 = x(0) - x_{\text{target}}$ and velocity $v_0 = v(0)$:

$$c_1 = y_0, \quad c_2 = \frac{v_0 + \zeta \omega_0 y_0}{\omega_d}$$

#### Regime 2: Critically Damped ($\zeta = 1.0$)

The characteristic equation has a repeated real root $s = -\omega_0$:

$$\begin{aligned}
x(t) &= x_{\text{target}} + e^{-\omega_0 t} \left( c_1 + c_2 t \right) \\
v(t) &= e^{-\omega_0 t} \left( c_2 (1 - \omega_0 t) - c_1 \omega_0 \right)
\end{aligned}$$

Constants derived from initial conditions:

$$c_1 = y_0, \quad c_2 = v_0 + \omega_0 y_0$$

#### Regime 3: Overdamped ($\zeta > 1.0$)

Two distinct real roots: $\lambda_1, \lambda_2 = -\omega_0 (\zeta \mp \sqrt{\zeta^2 - 1})$:

$$\begin{aligned}
x(t) &= x_{\text{target}} + c_1 e^{\lambda_1 t} + c_2 e^{\lambda_2 t} \\
c_1 &= \frac{v_0 - \lambda_2 y_0}{\lambda_1 - \lambda_2}, \quad c_2 = \frac{\lambda_1 y_0 - v_0}{\lambda_1 - \lambda_2}
\end{aligned}$$

---

## 5. Velocity Transfer & Momentum Preservation

Time-based curves (`cel_transition`) achieve $C^0$ positional continuity upon retargeting, but reset velocity to the curve's initial derivative:

$$\dot{x}(0^+) = \dot{f}(0) \cdot \frac{x_{\text{target, new}} - x_{\text{start}}}{\Delta t}$$

If $\dot{f}(0) = 0$ (e.g. Quad In, Sine In), in-flight velocity drops abruptly to zero, destroying physical momentum.

### 5.1 $C^1$ Velocity Continuity in Springs

Physical springs (`celm_spring`) achieve true **$C^1$ continuity** (smooth velocity transfer):

$$\begin{aligned}
x(0^+) &= x(t_{\text{retarget}}) \\
v(0^+) &= v(t_{\text{retarget}})
\end{aligned}$$

When the target shifts mid-motion, the existing instantaneous velocity $v(t_{\text{retarget}})$ is preserved directly into $v_0$ for the subsequent analytical evaluation step.

*Visual Outcome*: If an object is moving right at $800\,\text{px/s}$ and its target shifts left, the spring naturally absorbs the existing momentum, overshoots to the right while decelerating, and snaps back toward the new target without hitching.

---

## 6. Exponential Smoothing / Decay (`celm_damp`)

When second-order elastic overshoot is undesirable but frame-rate independent visual lag is required (e.g. camera following player, smooth scrolling):

### 6.1 Governing Differential Equation

$$\frac{dx}{dt} = -\lambda (x - x_{\text{target}})$$

where $\lambda > 0$ represents the convergence rate ($s^{-1}$).

### 6.2 Closed-Form Solution

Integrating over time step $\Delta t$:

$$x(t + \Delta t) = x_{\text{target}} + (x(t) - x_{\text{target}}) \cdot e^{-\lambda \Delta t}$$

### 6.3 Half-Life Relationship

The duration $t_{1/2}$ required for the remaining error distance to halve is:

$$e^{-\lambda t_{1/2}} = 0.5 \implies t_{1/2} = \frac{\ln 2}{\lambda} \approx \frac{0.69315}{\lambda}$$

#### Naive Lerp vs Exponential Smoothing

Naive frame-bound linear interpolation:
$$x_{k+1} = \text{lerp}(x_k, x_{\text{target}}, \alpha)$$
is severely frame-rate dependent: at 120 FPS it converges twice as fast as at 60 FPS.

Converting a 60 FPS coefficient $\alpha_{60}$ to a frame-rate independent rate $\lambda$:
$$\lambda = -60 \cdot \ln(1 - \alpha_{60})$$

---

## 7. Parametric Cubic Bézier Curves

For CSS-compatible curve trajectories (`cubic-bezier(x1, y1, x2, y2)`), curves are parameterized in 2D with control points $P_0 = (0, 0)$, $P_1 = (x_1, y_1)$, $P_2 = (x_2, y_2)$, $P_3 = (1, 1)$:

$$B(u) = (1 - u)^3 P_0 + 3(1 - u)^2 u P_1 + 3(1 - u) u^2 P_2 + u^3 P_3, \quad u \in [0, 1]$$

### 7.1 Coordinate Splitting

$$\begin{aligned}
x(u) &= 3(1 - u)^2 u x_1 + 3(1 - u) u^2 x_2 + u^3 \\
y(u) &= 3(1 - u)^2 u y_1 + 3(1 - u) u^2 y_2 + u^3
\end{aligned}$$

Given normalized progress $t \in [0, 1]$ along the horizontal axis, solving for parameter $u$ such that $x(u) = t$ requires numerical root finding.

### 7.2 Newton-Raphson Iterative Inversion

Derivative of $x$ with respect to $u$:

$$\frac{dx}{du} = 3(1 - u)^2 x_1 + 6(1 - u) u (x_2 - x_1) + 3u^2 (1 - x_2)$$

Starting from initial guess $u_0 = t$:

$$u_{k+1} = u_k - \frac{x(u_k) - t}{\left.\frac{dx}{du}\right|_{u_k}}$$

Iterating 4 to 6 steps converges to within single-precision float epsilon ($< 10^{-6}$). The final vertical position is evaluated as:

$$f(t) = y(u_{\text{solved}})$$

---

## 8. Convergence Conditions & The Zero-Idle Invariant

### 8.1 Numerical Settling Criteria

To prevent indefinite micro-oscillations from consuming CPU cycles, a motion primitive is classified as **settled** when both displacement and velocity fall below precision thresholds:

$$|x(t) - x_{\text{target}}| < \varepsilon_x \quad \text{and} \quad |v(t)| < \varepsilon_v$$

#### Standard Recommended Tolerances

| Parameter | Recommended Epsilon | Physical Significance |
| :--- | :--- | :--- |
| **Displacement ($\varepsilon_x$)** | $10^{-3}$ ($0.001\,\text{px}$ or $\text{units}$) | Sub-pixel imperceptible spatial error. |
| **Velocity ($\varepsilon_v$)** | $10^{-3}$ ($0.001\,\text{units/s}$) | Kinetic rest boundary. |

Upon meeting both criteria:
1. State is snapped to exact target: $x \leftarrow x_{\text{target}}$, $v \leftarrow 0.0$.
2. State flag is set: $\text{isSettled} \leftarrow \text{true}$.

### 8.2 The Zero-Idle Invariant

1. **Active Motion**: While $\text{isSettled} = \text{false}$, the motion primitive calls `CelsSessionInvalidateKey()` on each frame pass, ensuring the enclosing composable is recomposed on the next tick.
2. **Settled Rest**: As soon as $\text{isSettled} = \text{true}$, the primitive **ceases calling invalidation APIs**.
3. **0% CPU Idle**: When all composables settle, `CelsSessionRecompose()` encounters an empty invalidation queue, executing zero subtrees and dropping the host engine to 0% idle CPU overhead.
