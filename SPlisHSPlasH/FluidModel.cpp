#include "FluidModel.h"
#include "SPHKernels.h"
#include <iostream>
#include "TimeManager.h"
#include "TimeStep.h"
#include "Utilities/Logger.h"

using namespace SPH;
using namespace GenParam;

int FluidModel::KERNEL_METHOD = -1;
int FluidModel::GRAD_KERNEL_METHOD = -1;
int FluidModel::NUM_PARTICLES = -1;
int FluidModel::NUM_REUSED_PARTICLES = -1;
int FluidModel::PARTICLE_RADIUS = -1;
int FluidModel::DENSITY0 = -1;
int FluidModel::ENUM_KERNEL_CUBIC = -1;
int FluidModel::ENUM_KERNEL_POLY6 = -1;
int FluidModel::ENUM_KERNEL_SPIKY = -1;
int FluidModel::ENUM_KERNEL_PRECOMPUTED_CUBIC = -1;
int FluidModel::ENUM_GRADKERNEL_CUBIC = -1;
int FluidModel::ENUM_GRADKERNEL_POLY6 = -1;
int FluidModel::ENUM_GRADKERNEL_SPIKY = -1;
int FluidModel::ENUM_GRADKERNEL_PRECOMPUTED_CUBIC = -1;


FluidModel::FluidModel() :
	m_particleObjects(),
	m_masses(),
	m_a(),
	m_v0(),
	m_density()
{	
	m_emitterSystem = new EmitterSystem();
	m_density0 = 1000.0;
	setParticleRadius(0.025);
	m_neighborhoodSearch = NULL;	

	ParticleObject *fluidParticles = new ParticleObject();
	m_particleObjects.push_back(fluidParticles);

	m_kernelMethod = -1;
	m_gradKernelMethod = -1;
}

FluidModel::~FluidModel(void)
{
	cleanupModel();
}

void FluidModel::cleanupModel()
{
	releaseFluidParticles();
	for (unsigned int i = 0; i < m_particleObjects.size(); i++)
	{
		if (i > 0)
		{
			RigidBodyParticleObject *rbpo = ((RigidBodyParticleObject*)m_particleObjects[i]);
			rbpo->m_boundaryPsi.clear();
			rbpo->m_f.clear();
			delete rbpo->m_rigidBody;
			delete rbpo;
		}
		else
			delete m_particleObjects[i];
	}
	m_particleObjects.clear();

	m_v0.clear();
	m_a.clear();
	m_masses.clear();
	m_density.clear();
	delete m_neighborhoodSearch;
	delete m_emitterSystem;
}

void FluidModel::init()
{
	initParameters();
}

void FluidModel::initParameters()
{
	ParameterObject::initParameters();

	ParameterBase::GetFunc<Real> getRadiusFct = std::bind(&FluidModel::getParticleRadius, this);
	ParameterBase::SetFunc<Real> setRadiusFct = std::bind(&FluidModel::setParticleRadius, this, std::placeholders::_1);
	PARTICLE_RADIUS = createNumericParameter("particleRadius", "Particle radius", getRadiusFct, setRadiusFct);
	setGroup(PARTICLE_RADIUS, "Simulation");
	setDescription(PARTICLE_RADIUS, "Radius of the fluid particles.");
	getParameter(PARTICLE_RADIUS)->setReadOnly(true);

	ParameterBase::GetFunc<Real> getDensity0Fct = std::bind(&FluidModel::getDensity0, this);
	ParameterBase::SetFunc<Real> setDensity0Fct = std::bind(&FluidModel::setDensity0, this, std::placeholders::_1);
	DENSITY0 = createNumericParameter("density0", "Rest density", getDensity0Fct, setDensity0Fct);
	setGroup(DENSITY0, "Simulation");
	setDescription(DENSITY0, "Rest density of the fluid.");
	getParameter(DENSITY0)->setReadOnly(true);

	NUM_PARTICLES = createNumericParameter("numParticles", "# active particles", &m_numActiveParticles);
	setGroup(NUM_PARTICLES, "Simulation");
	setDescription(NUM_PARTICLES, "Number of active fluids particles in the simulation.");
	getParameter(NUM_PARTICLES)->setReadOnly(true);

	NUM_REUSED_PARTICLES = createNumericParameter<unsigned int>("numReusedParticles", "# reused particles", [&]() { return m_emitterSystem->numReusedParticles(); });
	setGroup(NUM_REUSED_PARTICLES, "Simulation");
	setDescription(NUM_REUSED_PARTICLES, "Number of reused fluids particles in the simulation.");
	getParameter(NUM_REUSED_PARTICLES)->setReadOnly(true);

	ParameterBase::GetFunc<int> getKernelFct = std::bind(&FluidModel::getKernel, this);
	ParameterBase::SetFunc<int> setKernelFct = std::bind(&FluidModel::setKernel, this, std::placeholders::_1);
	KERNEL_METHOD = createEnumParameter("kernel", "Kernel", getKernelFct, setKernelFct);
	setGroup(KERNEL_METHOD, "Kernel");
	setDescription(KERNEL_METHOD, "Kernel function used in the SPH model.");
	EnumParameter *enumParam = static_cast<EnumParameter*>(getParameter(KERNEL_METHOD));
	enumParam->addEnumValue("Cubic spline", ENUM_KERNEL_CUBIC);
	enumParam->addEnumValue("Poly6", ENUM_KERNEL_POLY6);
	enumParam->addEnumValue("Spiky", ENUM_KERNEL_SPIKY);
	enumParam->addEnumValue("Precomputed cubic spline", ENUM_KERNEL_PRECOMPUTED_CUBIC);

	ParameterBase::GetFunc<int> getGradKernelFct = std::bind(&FluidModel::getGradKernel, this);
	ParameterBase::SetFunc<int> setGradKernelFct = std::bind(&FluidModel::setGradKernel, this, std::placeholders::_1);
	GRAD_KERNEL_METHOD = createEnumParameter("gradKernel", "Gradient of kernel", getGradKernelFct, setGradKernelFct);
	setGroup(GRAD_KERNEL_METHOD, "Kernel");
	setDescription(GRAD_KERNEL_METHOD, "Gradient of the kernel function used in the SPH model.");
	enumParam = static_cast<EnumParameter*>(getParameter(GRAD_KERNEL_METHOD));
	enumParam->addEnumValue("Cubic spline", ENUM_GRADKERNEL_CUBIC);
	enumParam->addEnumValue("Poly6", ENUM_GRADKERNEL_POLY6);
	enumParam->addEnumValue("Spiky", ENUM_GRADKERNEL_SPIKY);
	enumParam->addEnumValue("Precomputed cubic spline", ENUM_GRADKERNEL_PRECOMPUTED_CUBIC);
}


void FluidModel::reset()
{
	m_emitterSystem->reset();
	setNumActiveParticles(m_numActiveParticles0);
	const unsigned int nPoints = numActiveParticles();

	// reset velocities and accelerations
	for (unsigned int i = 1; i < m_particleObjects.size(); i++)
	{
		for (int j = 0; j < (int)m_particleObjects[i]->m_x.size(); j++)
		{
			RigidBodyParticleObject *rbpo = ((RigidBodyParticleObject*)m_particleObjects[i]);
			rbpo->m_f[j].setZero();
			rbpo->m_v[j].setZero();
		}
	}
	
	if (m_neighborhoodSearch->point_set(0).n_points() != nPoints)
		m_neighborhoodSearch->resize_point_set(0, &getPosition(0, 0)[0], nPoints);

	// Fluid
	for (unsigned int i = 0; i < nPoints; i++)
	{
		const Vector3r& x0 = getPosition0(0, i);
		getPosition(0, i) = x0;
		getVelocity(0, i) = getVelocity0(i);
		getAcceleration(i).setZero();
		m_density[i] = 0.0;
	}

	updateBoundaryPsi();
}

void FluidModel::initMasses()
{
	const int nParticles = (int) numParticles();
	const Real diam = 2.0*m_particleRadius;

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < nParticles; i++)
		{
			setMass(i, 0.8 * diam*diam*diam * m_density0);		// each particle represents a cube with a side length of r		
																// mass is slightly reduced to prevent pressure at the beginning of the simulation
		}
	}
}

void FluidModel::resizeFluidParticles(const unsigned int newSize)
{
	m_particleObjects[0]->m_x0.resize(newSize);
	m_particleObjects[0]->m_x.resize(newSize);
	m_particleObjects[0]->m_v.resize(newSize);
	m_v0.resize(newSize);
	m_a.resize(newSize);
	m_masses.resize(newSize);
	m_density.resize(newSize);
}

void FluidModel::releaseFluidParticles()
{
	m_particleObjects[0]->m_x0.clear();
	m_particleObjects[0]->m_x.clear();
	m_particleObjects[0]->m_v.clear();
	m_v0.clear();
	m_a.clear();
	m_masses.clear();
	m_density.clear();
}

void FluidModel::initModel(const unsigned int nFluidParticles, Vector3r* fluidParticles, Vector3r* fluidVelocities, const unsigned int nMaxEmitterParticles)
{
	releaseFluidParticles();
	resizeFluidParticles(nFluidParticles + nMaxEmitterParticles);

	// init kernel
	setParticleRadius(m_particleRadius);

	// make sure that m_W_zero is set correctly for the new particle radius
	setKernel(getKernel());

	// copy fluid positions
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < (int)nFluidParticles; i++)
		{
			getPosition0(0, i) = fluidParticles[i];
			getVelocity0(i) = fluidVelocities[i];
		}
	}

	// initialize masses
	initMasses();

	// Initialize neighborhood search
	if (m_neighborhoodSearch == NULL)
		m_neighborhoodSearch = new CompactNSearch::NeighborhoodSearch(m_supportRadius, false);
	m_neighborhoodSearch->set_radius(m_supportRadius);

	// Fluids 
	m_neighborhoodSearch->add_point_set(&getPosition(0, 0)[0], nFluidParticles, true, true);

	// Boundary
	for (unsigned int i = 0; i < numberOfRigidBodyParticleObjects(); i++)
	{
		RigidBodyParticleObject *rb = getRigidBodyParticleObject(i);
		m_neighborhoodSearch->add_point_set(&rb->m_x[0][0], rb->m_x.size(), rb->m_rigidBody->isDynamic(), false);
	}

	m_numActiveParticles0 = nFluidParticles;
	m_numActiveParticles = m_numActiveParticles0;

	reset();
}

void FluidModel::updateBoundaryPsi()
{
	if (m_neighborhoodSearch == nullptr)
		return; 

	//////////////////////////////////////////////////////////////////////////
	// Compute value psi for boundary particles (boundary handling)
	// (see Akinci et al. "Versatile rigid - fluid coupling for incompressible SPH", Siggraph 2012
	//////////////////////////////////////////////////////////////////////////

	// Search boundary neighborhood

	// Activate only static boundaries
	LOG_INFO << "Initialize boundary psi";
	m_neighborhoodSearch->set_active(false);
	for (unsigned int i = 0; i < numberOfRigidBodyParticleObjects(); i++)
	{
		if (!getRigidBodyParticleObject(i)->m_rigidBody->isDynamic())
			m_neighborhoodSearch->set_active(i + 1, true, true);
	}

	m_neighborhoodSearch->find_neighbors();

	// Boundary objects
	for (unsigned int body = 0; body < numberOfRigidBodyParticleObjects(); body++)
	{
		if (!getRigidBodyParticleObject(body)->m_rigidBody->isDynamic())
			computeBoundaryPsi(body);
	}

	////////////////////////////////////////////////////////////////////////// 
	// Compute boundary psi for all dynamic bodies
	//////////////////////////////////////////////////////////////////////////
	for (unsigned int body = 0; body < numberOfRigidBodyParticleObjects(); body++)
	{
		// Deactivate all
		m_neighborhoodSearch->set_active(false);

		// Only activate next dynamic body
		if (getRigidBodyParticleObject(body)->m_rigidBody->isDynamic())
		{
			m_neighborhoodSearch->set_active(body + 1, true, true);
			m_neighborhoodSearch->find_neighbors();
			computeBoundaryPsi(body);
		}
	}

	// Activate only fluids 	
	m_neighborhoodSearch->set_active(false); 	
	m_neighborhoodSearch->set_active(0u, 0u, true); 	
	for (unsigned int i = 1; i < m_neighborhoodSearch->point_sets().size(); i++) 		
		m_neighborhoodSearch->set_active(0u, i, true);
}

void FluidModel::computeBoundaryPsi(const unsigned int body)
{
	const Real density0 = getDensity0();
	
	RigidBodyParticleObject *rb = getRigidBodyParticleObject(body);
	const unsigned int numBoundaryParticles = rb->numberOfParticles();

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < (int)numBoundaryParticles; i++)
		{
			Real delta = m_W_zero;
			for (unsigned int pid = 1; pid < numberOfPointSets(); pid++)
			{
				for (unsigned int j = 0; j < m_neighborhoodSearch->point_set(body + 1).n_neighbors(pid, i); j++)
				{
					const unsigned int neighborIndex = m_neighborhoodSearch->point_set(body + 1).neighbor(pid, i, j);
					delta += W(getPosition(body + 1, i) - getPosition(pid, neighborIndex));
				}
			}
			const Real volume = 1.0 / delta;
			rb->m_boundaryPsi[i] = density0 * volume; 
		}
	}
}

void FluidModel::addRigidBodyObject(RigidBodyObject *rbo, const unsigned int numBoundaryParticles, Vector3r *boundaryParticles)
{
	RigidBodyParticleObject *rb = new RigidBodyParticleObject();
	m_particleObjects.push_back(rb);

	rb->m_x0.resize(numBoundaryParticles);
	rb->m_x.resize(numBoundaryParticles);
	rb->m_v.resize(numBoundaryParticles);
	rb->m_f.resize(numBoundaryParticles);
	rb->m_boundaryPsi.resize(numBoundaryParticles);

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < (int) numBoundaryParticles; i++)
		{
			rb->m_x0[i] = boundaryParticles[i];
			rb->m_x[i] = boundaryParticles[i];
			rb->m_v[i].setZero();
			rb->m_f[i].setZero();
		}
	}
	rb->m_rigidBody = rbo;
}

void FluidModel::performNeighborhoodSearchSort()
{
	const unsigned int numPart = numActiveParticles();
	if (numPart == 0)
		return;

	m_neighborhoodSearch->z_sort();

	auto const& d = m_neighborhoodSearch->point_set(0);
	d.sort_field(&m_particleObjects[0]->m_x[0]);
	d.sort_field(&m_particleObjects[0]->m_v[0]);
	d.sort_field(&m_a[0]);
	d.sort_field(&m_masses[0]);
	d.sort_field(&m_density[0]);


	//////////////////////////////////////////////////////////////////////////
	// Boundary
	//////////////////////////////////////////////////////////////////////////
	for (unsigned int i = 1; i < m_neighborhoodSearch->point_sets().size(); i++)
	{
		RigidBodyParticleObject *rb = getRigidBodyParticleObject(i - 1);
		if (rb->m_rigidBody->isDynamic())			// sort only dynamic boundaries
		{
			auto const& d = m_neighborhoodSearch->point_set(i);
			d.sort_field(&rb->m_x[0]);
			d.sort_field(&rb->m_v[0]);
			d.sort_field(&rb->m_f[0]);
			d.sort_field(&rb->m_boundaryPsi[0]);
		}
	}
}

void SPH::FluidModel::setDensity0(const Real v)
{
	m_density0 = v; 
	initMasses(); 
	updateBoundaryPsi();
}

void FluidModel::setParticleRadius(Real val)
{
	m_particleRadius = val; 
	m_supportRadius = 4.0*m_particleRadius;

	// init kernel
	Poly6Kernel::setRadius(m_supportRadius);
	SpikyKernel::setRadius(m_supportRadius);
	CubicKernel::setRadius(m_supportRadius);
	PrecomputedCubicKernel::setRadius(m_supportRadius);
	CohesionKernel::setRadius(m_supportRadius);
	AdhesionKernel::setRadius(m_supportRadius);
}


void SPH::FluidModel::setGradKernel(int val)
{
	m_gradKernelMethod = val;
	if (m_gradKernelMethod == 0)
		m_gradKernelFct = CubicKernel::gradW;
	else if (m_gradKernelMethod == 1)
		m_gradKernelFct = Poly6Kernel::gradW;
	else if (m_gradKernelMethod == 2)
		m_gradKernelFct = SpikyKernel::gradW;
	else if (m_gradKernelMethod == 3)
		m_gradKernelFct = FluidModel::PrecomputedCubicKernel::gradW;
}

void SPH::FluidModel::setKernel(int val)
{
	const auto old_kernelMethod = m_kernelMethod;
	const auto old_W_Zero = m_W_zero;

	m_kernelMethod = val;
	if (m_kernelMethod == 0)
	{
		m_W_zero = CubicKernel::W_zero();
		m_kernelFct = CubicKernel::W;
	}
	else if (m_kernelMethod == 1)
	{
		m_W_zero = Poly6Kernel::W_zero();
		m_kernelFct = Poly6Kernel::W;
	}
	else if (m_kernelMethod == 2)
	{
		m_W_zero = SpikyKernel::W_zero();
		m_kernelFct = SpikyKernel::W;
	}
	else if (m_kernelMethod == 3)
	{
		m_W_zero = FluidModel::PrecomputedCubicKernel::W_zero();
		m_kernelFct = FluidModel::PrecomputedCubicKernel::W;
	}

	if (old_kernelMethod != m_kernelMethod || old_W_Zero != m_W_zero)
		updateBoundaryPsi();
}

void FluidModel::setNumActiveParticles(const unsigned int num)
{
	m_numActiveParticles = num;
}

unsigned int FluidModel::numActiveParticles() const
{
	return m_numActiveParticles;
}